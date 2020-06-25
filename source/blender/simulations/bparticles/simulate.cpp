#include "DNA_object_force_types.h"

#include "BLI_timeit.h"
#include "BLI_array_cxx.h"
#include "BLI_vector_adaptor.h"
#include "BLI_parallel.h"

#include "BKE_collision.h"
#include "BKE_modifier.h"

#include "FN_cpp_type.h"

#include "simulate.hpp"

namespace BParticles {

using BLI::ScopedVector;
using BLI::Vector;
using BLI::VectorAdaptor;
using FN::CPPType;

/************************************************
 * Collisions (taken from the old particle_system.c)
 *
 * The algorithm is roughly:
 *  1. Use a BVH tree to search for faces that a particle may collide with.
 *  2. Use Newton's method to find the exact time at which the collision occurs.
 *     https://en.wikipedia.org/wiki/Newton's_method
 *
 ************************************************/
#define COLLISION_MIN_RADIUS 0.001f     // TODO check if this is needed
#define COLLISION_MIN_DISTANCE 0.0001f  // TODO check if this is needed
#define COLLISION_ZERO 0.00001f

static void normal_from_closest_point_to_tri(
    float no[3], const float p[3], const float v0[3], const float v1[3], const float v2[3])
{
  // Calculate the normal using the closest point on the triangle. This makes sure that
  // particles can collide and be deflected in the correct direction when colliding with verts
  // or edges of the triangle.
  float point_on_tri[3];
  closest_on_tri_to_point_v3(point_on_tri, p, v0, v1, v2);
  sub_v3_v3v3(no, p, point_on_tri);
  normalize_v3(no);
}

static float distance_to_tri(float3 &p, std::array<float3, 3> &cur_tri_points, float radius)
{
  float3 closest_point;
  closest_on_tri_to_point_v3(
      closest_point, p, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);

  return float3::distance(closest_point, p) - radius;
}

static void collision_interpolate_element(std::array<std::pair<float3, float3>, 3> &tri_points,
                                          std::array<float3, 3> &cur_tri_points,
                                          float t)
{
  for (int i = 0; i < tri_points.size(); i++) {
    cur_tri_points[i] = float3::interpolate(tri_points[i].first, tri_points[i].second, t);
  }
}

static void calc_hit_point_data_tri(float co[3],
                                    float no[3],
                                    float w[3],
                                    const float v0[3],
                                    const float v1[3],
                                    const float v2[3],
                                    float offset)
{
  bool inside = true;

  interp_weights_tri_v3(w, v0, v1, v2, co);

  for (int i = 0; i < 3; i++) {
    if (w[i] < 0.0f || w[i] > 1.0f) {
      inside = false;
      break;
    }
  }
  // Calculate normal of the point we hit.
  if (inside) {
    // We are inside the triangle, use the triangle normal as this is more accurate than the
    // calulated one below.
    normal_tri_v3(no, v0, v1, v2);
    // Make sure the normal is pointing in the right direction
    float3 point_vec;
    sub_v3_v3v3(point_vec, co, v0);
    if (dot_v3v3(no, point_vec) < 0.0f) {
      mul_v3_fl(no, -1.0f);
    }
  }
  else {
    normal_from_closest_point_to_tri(no, co, v0, v1, v2);
  }

  // Calcualte a point that is not directly in contact with the current triangle. This is so we do
  // not stick to the surface as it will collide with the same triangle immediately next time we
  // check, even if it moving away from it.
  // The offset should be greater than the particle radius
  float3 point = co;
  float3 normal = no;
  float3 p_on_tri;
  closest_on_tri_to_point_v3(p_on_tri, point, v0, v1, v2);

  point = p_on_tri + normal * offset;
  copy_v3_v3(co, point);
}

/* find first root in range [0-1] starting from 0 */
static float collision_newton_rhapson(std::pair<float3, float3> &particle_points,
                                      std::array<std::pair<float3, float3>, 3> &tri_points,
                                      float radius,
                                      float radius_epsilon,
                                      float3 &coll_normal,
                                      float3 &hit_bary_weights,
                                      float3 &point_on_plane)
{
  std::array<float3, 3> cur_tri_points;
  float t0, t1, dt_init, d0, d1, dd;
  float3 p;

  dt_init = 0.001f;
  /* start from the beginning */
  t0 = 0.f;
  collision_interpolate_element(tri_points, cur_tri_points, t0);
  d0 = distance_to_tri(particle_points.first, cur_tri_points, radius);
  t1 = dt_init;
  d1 = 0.f;

  /* particle already inside face, so report collision */
  if (d0 <= COLLISION_ZERO) {
    p = particle_points.first;

    calc_hit_point_data_tri(p,
                            coll_normal,
                            hit_bary_weights,
                            cur_tri_points[0],
                            cur_tri_points[1],
                            cur_tri_points[2],
                            radius + radius_epsilon);
    point_on_plane = p;

    return 0.f;
  }

  for (int iter = 0; iter < 10; iter++) {  //, itersum++) {
    /* get current location */
    collision_interpolate_element(tri_points, cur_tri_points, t1);
    p = float3::interpolate(particle_points.first, particle_points.second, t1);

    d1 = distance_to_tri(p, cur_tri_points, radius);

    /* Zero gradient (no movement relative to element). Can't step from
     * here. */
    if (d1 == d0) {
      /* If first iteration, try from other end where the gradient may be
       * greater. Note: code duplicated below. */
      if (iter == 0) {
        t0 = 1.f;
        collision_interpolate_element(tri_points, cur_tri_points, t0);
        d0 = distance_to_tri(particle_points.second, cur_tri_points, radius);
        t1 = 1.0f - dt_init;
        d1 = 0.f;
        continue;
      }
      else {
        return -1.f;
      }
    }

    if (d1 <= COLLISION_ZERO) {
      if (t1 >= -COLLISION_ZERO && t1 <= 1.f) {

        calc_hit_point_data_tri(p,
                                coll_normal,
                                hit_bary_weights,
                                cur_tri_points[0],
                                cur_tri_points[1],
                                cur_tri_points[2],
                                radius + radius_epsilon);
        point_on_plane = p;

        CLAMP(t1, 0.f, 1.f);
        return t1;
      }
      else {
        return -1.f;
      }
    }

    /* Derive next time step */
    dd = (t1 - t0) / (d1 - d0);

    t0 = t1;
    d0 = d1;

    t1 -= d1 * dd;

    /* Particle moving away from plane could also mean a strangely rotating
     * face, so check from end. Note: code duplicated above. */
    if (iter == 0 && t1 < 0.f) {
      t0 = 1.f;
      collision_interpolate_element(tri_points, cur_tri_points, t0);
      d0 = distance_to_tri(particle_points.second, cur_tri_points, radius);
      t1 = 1.0f - dt_init;
      d1 = 0.f;
      continue;
    }
    else if (iter == 1 && (t1 < -COLLISION_ZERO || t1 > 1.f)) {
      return -1.f;
    }
  }
  return -1.0;
}

typedef struct RayCastData {
  std::pair<float3, float3> particle_points;
  CollisionModifierData *collmd;
  float3 hit_vel;
  float duration;
  float start_time;
  float radius_epsilon;
  float rel_dt;  // A relative number between 0.0f-1.0f
} RayCastData;

BLI_NOINLINE static void raycast_callback(void *userdata,
                                          int index,
                                          const BVHTreeRay *ray,
                                          BVHTreeRayHit *hit)
{
  RayCastData *rd = (RayCastData *)userdata;
  CollisionModifierData *collmd = rd->collmd;

  const MVertTri *vt = &collmd->tri[index];
  MVert *verts = collmd->x;
  const float *v0, *v1, *v2;
  float dist;

  v0 = verts[vt->tri[0]].co;
  v1 = verts[vt->tri[1]].co;
  v2 = verts[vt->tri[2]].co;

  if (collmd->is_static) {
    zero_v3(rd->hit_vel);

    if (ray->radius == 0.0f) {
      // TODO particles probably need to always have somekind of radius, so this can probably be
      // removed after testing is done.
      dist = bvhtree_ray_tri_intersection(ray, hit->dist, v0, v1, v2);
    }
    else {
      dist = bvhtree_sphereray_tri_intersection(ray, ray->radius, hit->dist, v0, v1, v2);
    }

    // TODO perhaps check if the new collision is inside the triangle and the distance is within
    // COLLISION_MIN_DISTANCE, then we take the collision that is inside the face. Note that this
    // only for the same coll object

    if (dist >= 0.0f && dist < hit->dist) {
      hit->index = index;
      hit->dist = dist;
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);

      float3 w;

      calc_hit_point_data_tri(hit->co, hit->no, w, v0, v1, v2, ray->radius + rd->radius_epsilon);
      // No dt info available for static collisions, will manually calculate this later.
      rd->rel_dt = 0.0f;
    }
    return;
  }

  MVert *new_verts = collmd->xnew;
  const float *v0_new, *v1_new, *v2_new;
  v0_new = new_verts[vt->tri[0]].co;
  v1_new = new_verts[vt->tri[1]].co;
  v2_new = new_verts[vt->tri[2]].co;

  std::array<std::pair<float3, float3>, 3> tri_points;
  float3 coll_normal;
  float3 hit_bary_weights;

  tri_points[0] = std::pair<float3, float3>(v0, v0_new);
  tri_points[1] = std::pair<float3, float3>(v1, v1_new);
  tri_points[2] = std::pair<float3, float3>(v2, v2_new);

  if (rd->start_time != 0.0f) {
    // Adjust the triangle start positions
    std::array<float3, 3> new_start_points;
    collision_interpolate_element(tri_points, new_start_points, rd->start_time);
    tri_points[0].first = new_start_points[0];
    tri_points[1].first = new_start_points[1];
    tri_points[2].first = new_start_points[2];
  }

  float3 point_on_plane;
  /* TODO this is to silence gcc "may be use un-intied" warnings. Look into if I missed a case
   * where this is actually true */
  zero_v3(point_on_plane);

  // Check if we get hit by the moving object
  float coll_time = collision_newton_rhapson(rd->particle_points,
                                             tri_points,
                                             ray->radius,
                                             rd->radius_epsilon,
                                             coll_normal,
                                             hit_bary_weights,
                                             point_on_plane);

  dist = float3::distance(rd->particle_points.first, rd->particle_points.second) * coll_time;

  if (coll_time >= 0.f) {
    if (hit->index != -1 && dist >= 0.0f && dist >= hit->dist) {
      /* We have already collided with an other object at closer distance */
      return;
    }
    // We have a collision!
    hit->index = index;
    hit->dist = dist;
    rd->rel_dt = coll_time;

    // TODO might need to derive the velocity from acceleration to avoid "staircase effects" on
    // moving colliders

    // Calculate the velocity of the point we hit
    zero_v3(rd->hit_vel);
    for (int i = 0; i < 3; i++) {
      // Make sure that all the weights are between 0-1. They can be negative or above 1 if the
      // point lies slightly outside of the triangle.
      CLAMP(hit_bary_weights[i], 0.f, 1.f);

      rd->hit_vel += (tri_points[i].second - tri_points[i].first) * hit_bary_weights[i] /
                     rd->duration;
    }

    copy_v3_v3(hit->co, point_on_plane);
    copy_v3_v3(hit->no, coll_normal);
  }
}

// TODO come up with a better function name...
static float3 min_add(float3 a, float3 b)
{

  if (dot_v3v3(a, b) == -1.0f) {
    // If a == -b then we will get NaN in this function.
    // So just return a and hope for the best.
    return a;
  }

  if (is_zero_v3(a)) {
    return b;
  }

  if (is_zero_v3(b)) {
    return a;
  }

  if (dot_v3v3(a, b) < 0.0f) {
    a -= float3::project(a, b);
    b -= float3::project(b, a);
  }
  float3 proj = float3::project(a, b);

  if (proj.length() > b.length()) {
    // Make sure we use the longest one as basis.
    float3 temp = a;
    a = b;
    b = temp;
    proj = float3::project(a, b);
  }

  b += a - proj;

  return b;
}

BLI_NOINLINE static void simulate_particle_chunk(SimulationState &UNUSED(simulation_state),
                                                 ParticleAllocator &UNUSED(particle_allocator),
                                                 MutableAttributesRef attributes,
                                                 ParticleSystemInfo &system_info,
                                                 MutableArrayRef<float> remaining_durations,
                                                 float UNUSED(end_time),
                                                 ArrayRef<ColliderCache *> colliders)
{
  uint amount = attributes.size();
  BLI_assert(amount == remaining_durations.size());

  BufferCache buffer_cache;

  Array<float3> forces(attributes.size(), {0, 0, 0});
  for (Force *force : system_info.forces) {
    force->add_force(attributes, IndexRange(amount), buffer_cache, forces);
  }

  MutableArrayRef<float3> velocities = attributes.get<float3>("Velocity");
  MutableArrayRef<float3> positions = attributes.get<float3>("Position");
  MutableArrayRef<float> sizes = attributes.get<float>("Size");
  // MutableArrayRef<bool> dead_state = attributes.get<bool>("Dead");

  for (uint pindex : IndexRange(amount)) {
    // if (pindex != 422) {
    //  continue;
    //}

    // if (positions[pindex][2] > -0.7f) {
    //  printf("pindex: %d\n", pindex);
    //}

    float mass = 1.0f;
    float duration = remaining_durations[pindex];
    bool collided;
    int coll_num = 0;

    float3 constraint_velo = float3(0.0f);

    // Check if any 'collobjs' collide with the particles here
    if (colliders.size() != 0) {
      CollisionModifierData *prev_collider = NULL;
      int prev_hit_idx = -1;

      do {
        BVHTreeRayHit best_hit;
        float3 best_hit_vel;
        PartDeflect *best_hit_settings;
        float best_dt;
        float max_move;

        float3 dir;

        if (is_zero_v3(velocities[pindex])) {
          // If velocity is zero, then no collisions will be detected with moving colliders. Make
          // sure that we have a forced check by setting the dir to something that is not zero.
          dir = float3(0, 0, 1.0f);
          max_move = FLT_EPSILON;
        }
        else {
          dir = velocities[pindex].normalized();
          max_move = (duration * velocities[pindex]).length();
        }

        best_hit.dist = FLT_MAX;
        collided = false;

        for (ColliderCache *col : colliders) {
          CollisionModifierData *collmd = col->collmd;

          if (!collmd->bvhtree) {
            continue;
          }

          const int raycast_flag = BVH_RAYCAST_DEFAULT;

          BVHTreeRayHit hit;
          hit.index = -1;
          hit.dist = max_move;

          float particle_radius = sizes[pindex];

          float3 start = positions[pindex];

          RayCastData rd;

          rd.collmd = collmd;
          rd.particle_points.first = start;
          rd.particle_points.second = start + duration * velocities[pindex];

          rd.duration = duration;
          rd.start_time = 1.0 - duration / remaining_durations[pindex];
          rd.radius_epsilon = (1.0f + 10.0f * floor(coll_num / 5.0f)) * COLLISION_MIN_DISTANCE;

          // TODO perhaps have two callbacks and check for static colider here instead?
          // So, if static use callback A otherwise B
          BLI_bvhtree_ray_cast_ex(collmd->bvhtree,
                                  start,
                                  dir,
                                  particle_radius,
                                  &hit,
                                  raycast_callback,
                                  &rd,
                                  raycast_flag);

          if (hit.index == -1 || best_hit.dist < hit.dist) {
            // We didn't hit anything
            continue;
          }
          if (collmd->is_static && prev_collider == collmd && prev_hit_idx == hit.index) {
            // We collided with the same face twice in a row.
            // Skip collision handling here as the set velocity from the previous collision
            // handling should keep the particle from tunneling through the face.
            // (If it is static, otherwise dampening and friction might make it collide again
            // during the same time step)
            continue;
          }

          best_hit = hit;
          best_hit_vel = rd.hit_vel;
          best_hit_settings = col->ob->pd;

          prev_collider = collmd;
          prev_hit_idx = hit.index;

          if (collmd->is_static) {
            best_dt = duration * (best_hit.dist / max_move);
          }
          else {
            best_dt = duration * rd.rel_dt;
          }

          collided = true;
        }
        if (collided) {
          // Calculate the remaining duration
          // printf("old dur: %f\n", duration);
          duration -= best_dt;
          // printf("new dur: %f\n", duration);

          // Update the current velocity from forces
          velocities[pindex] += best_dt * forces[pindex] * mass;

          // dead_state[pindex] = true;
          // TODO rename "dampening, in the old particle system dampening here was used to only
          // reduce the speed in the normal direction. So a better name would be bouncyness or
          // elastisity.
          float dampening = best_hit_settings->pdef_damp;
          float friction = best_hit_settings->pdef_frict;

          float3 normal = best_hit.no;

          float dot_epsilon = 1e-5f;

          // Modify constraint_velo so if it is along the collider normal if it is moving into
          // the collision plane.
          if (dot_v3v3(constraint_velo, normal) < -dot_epsilon) {
            float len = constraint_velo.length();

            constraint_velo -= float3::project(constraint_velo, normal);

            // Make sure that we are moving the same amount as before, otherwise this will cause
            // the constraint to lose the desired final speed and the particle will possibly not
            // move enough.
            constraint_velo *= len / constraint_velo.length();
          }

          if (dot_v3v3(best_hit_vel, normal) > dot_epsilon) {
            // The collider is moving towards the particle, we need to make sure that the particle
            // has enough velocity to not tunnel through.
            // The minimal distance we have to travel to still be outside is in the normal
            // direction. (Disregarding any other colliders of course...)
            float3 min_move = float3::project(best_hit_vel, normal);

            constraint_velo = min_add(constraint_velo, min_move);
          }

          float3 hit_velo_normal = float3::project(best_hit_vel, normal);
          float3 hit_velo_tangent = best_hit_vel - hit_velo_normal;

          float3 part_velo_normal = float3::project(velocities[pindex], normal);
          float3 part_velo_tangent = velocities[pindex] - part_velo_normal;

          interp_v3_v3v3(part_velo_tangent, part_velo_tangent, hit_velo_tangent, friction);

          float3 deflect_vel = part_velo_tangent -
                               (part_velo_normal - hit_velo_normal) * (1.0f - dampening);

          if (dot_v3v3(hit_velo_normal, part_velo_normal) > dot_epsilon) {
            // The collider were traveling in the same direction as the particle.
            // We need to add the initial particle velocity back (in the normal direction) to get
            // the final velocity.
            // Otherwise, we would only get how much speed is gained from the collision.
            deflect_vel += part_velo_normal;
          }

          // print_v3("normal", normal);
          // print_v3("best hit vel", best_hit_vel);
          // print_v3("pos", positions[pindex]);
          // print_v3("velo", velocities[pindex]);
          // print_v3("deflect_vel", deflect_vel);
          // print_v3("const vel", constraint_velo);
          // printf("\n");

          if (!is_zero_v3(constraint_velo)) {
            if (coll_num == 99) {
              // If we are at the last collision check, just try to go into the constraint velocity
              // direction and hope for the best.
              deflect_vel = constraint_velo;
            }
            else if (float3::project(deflect_vel, constraint_velo).length() <
                     constraint_velo.length()) {
              // We are not moving out the required amount, try to fix that.
              deflect_vel = min_add(deflect_vel, constraint_velo);
            }
          }

          positions[pindex] = best_hit.co;
          velocities[pindex] = deflect_vel;

          // printf("pindex: %d collnum: %d hit_idx %d\n\n", pindex, coll_num, prev_hit_idx);
          coll_num++;
        }
      } while (collided && coll_num < 100);
      // TODO perhaps expose the max iterations in the UI?
    }
    float3 move_vec = duration * velocities[pindex];
    positions[pindex] += move_vec;
    // Apply forces
    velocities[pindex] += duration * forces[pindex] * mass;

    // print_v3("final_velo", velocities[pindex]);
    // printf("dur: %f\n", duration);
    // print_v3("move_vec", move_vec);
    // print_v3("final_pos", positions[pindex]);
  }
}

BLI_NOINLINE static void delete_tagged_particles_and_reorder(ParticleSet &particles)
{
  auto kill_states = particles.attributes().get<bool>("Dead");
  ScopedVector<uint> indices_to_delete;

  for (uint i : kill_states.index_range()) {
    if (kill_states[i]) {
      indices_to_delete.append(i);
    }
  }

  particles.destruct_and_reorder(indices_to_delete.as_ref());
}

BLI_NOINLINE static void simulate_particles_for_time_span(SimulationState &simulation_state,
                                                          ParticleAllocator &particle_allocator,
                                                          ParticleSystemInfo &system_info,
                                                          FloatInterval time_span,
                                                          MutableAttributesRef particle_attributes)
{
  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    Array<float> remaining_durations(range.size(), time_span.size());
    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            time_span.end(),
                            colliders);
  });

  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_particles_from_birth_to_end_of_step(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    ParticleSystemInfo &system_info,
    float end_time,
    MutableAttributesRef particle_attributes)
{
  ArrayRef<float> all_birth_times = particle_attributes.get<float>("Birth Time");

  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    ArrayRef<float> birth_times = all_birth_times.slice(range);

    Array<float> remaining_durations(range.size());
    for (uint i : remaining_durations.index_range()) {
      remaining_durations[i] = end_time - birth_times[i];
    }

    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            end_time,
                            colliders);
  });
  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_existing_particles(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  BLI::parallel_map_items(simulation_state.particles().particle_containers(),
                          [&](StringRef system_name, ParticleSet *particle_set) {
                            ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(
                                system_name);
                            if (system_info == nullptr) {
                              return;
                            }

                            simulate_particles_for_time_span(simulation_state,
                                                             particle_allocator,
                                                             *system_info,
                                                             simulation_time_span,
                                                             particle_set->attributes());
                          });
}

BLI_NOINLINE static void create_particles_from_emitters(SimulationState &simulation_state,
                                                        ParticleAllocator &particle_allocator,
                                                        ArrayRef<Emitter *> emitters,
                                                        FloatInterval time_span)
{
  BLI::parallel_for(emitters.index_range(), [&](uint emitter_index) {
    Emitter &emitter = *emitters[emitter_index];
    EmitterInterface interface(simulation_state, particle_allocator, time_span);
    emitter.emit(interface);
  });
}

void simulate_particles(SimulationState &simulation_state,
                        ArrayRef<Emitter *> emitters,
                        StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  // SCOPED_TIMER(__func__);

  // systems_to_simulate.foreach_item([](StringRef name, ParticleSystemInfo &system_info) {
  //  system_info.collision_objects.print_as_lines(
  //      name, [](const CollisionObject &collision_object) {
  //        std::cout << collision_object.object->id.name
  //                  << " - Damping: " << collision_object.damping << " - Location Old: "
  //                  << float3(collision_object.local_to_world_start.values[3])
  //                  << " - Location New: "
  //                  << float3(collision_object.local_to_world_end.values[3]);
  //      });
  //});

  ParticlesState &particles_state = simulation_state.particles();
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  StringMultiMap<ParticleSet *> all_newly_created_particles;
  StringMultiMap<ParticleSet *> newly_created_particles;
  {
    ParticleAllocator particle_allocator(particles_state);
    BLI::parallel_invoke(
        [&]() {
          simulate_existing_particles(simulation_state, particle_allocator, systems_to_simulate);
        },
        [&]() {
          create_particles_from_emitters(
              simulation_state, particle_allocator, emitters, simulation_time_span);
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles = newly_created_particles;
  }

  while (newly_created_particles.key_amount() > 0) {
    ParticleAllocator particle_allocator(particles_state);

    BLI::parallel_map_items(
        newly_created_particles, [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
          ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(name);
          if (system_info == nullptr) {
            return;
          }

          BLI::parallel_for(new_particle_sets.index_range(), [&](uint index) {
            ParticleSet &particle_set = *new_particle_sets[index];
            simulate_particles_from_birth_to_end_of_step(simulation_state,
                                                         particle_allocator,
                                                         *system_info,
                                                         simulation_time_span.end(),
                                                         particle_set.attributes());
          });
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles.add_multiple(newly_created_particles);
  }

  BLI::parallel_map_items(all_newly_created_particles,
                          [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
                            ParticleSet &main_set = particles_state.particle_container(name);

                            for (ParticleSet *set : new_particle_sets) {
                              main_set.add_particles(*set);
                              delete set;
                            }
                          });

  BLI::parallel_map_keys(systems_to_simulate, [&](StringRef name) {
    ParticleSet &particles = particles_state.particle_container(name);
    delete_tagged_particles_and_reorder(particles);
  });
}

}  // namespace BParticles
