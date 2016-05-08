#include "nanort.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

// PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)
// http://www.pcg-random.org/
typedef struct {
  unsigned long long state;
  unsigned long long inc; // not used?
} pcg32_state_t;

#define PCG32_INITIALIZER                                                      \
  { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL }

float pcg32_random(pcg32_state_t *rng) {
  unsigned long long oldstate = rng->state;
  rng->state = oldstate * 6364136223846793005ULL + rng->inc;
  unsigned int xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
  unsigned int rot = oldstate >> 59u;
  unsigned int ret = (xorshifted >> rot) | (xorshifted << ((-rot) & 31));

  return (float)((double)ret / (double)4294967296.0);
}

void pcg32_srandom(pcg32_state_t *rng, uint64_t initstate, uint64_t initseq) {
  rng->state = 0U;
  rng->inc = (initseq << 1U) | 1U;
  pcg32_random(rng);
  rng->state += initstate;
  pcg32_random(rng);
}

unsigned char fclamp(float x) {
  int i = (int)(powf(x, 1.0 / 2.2) * 256.0f);
  if (i > 255)
    i = 255;
  if (i < 0)
    i = 0;

  return (unsigned char)i;
}

void SaveImagePNG(const char *filename, const float *rgb, int width,
                  int height) {

  std::vector<unsigned char> ldr(width * height * 3);
  for (size_t i = 0; i < (size_t)(width * height * 3); i++) {
    ldr[i] = fclamp(rgb[i]);
  }

  int len = stbi_write_png(filename, width, height, 3, &ldr.at(0), width * 3);
  if (len < 1) {
    printf("Failed to save image\n");
    exit(-1);
  }
}

// Predefined SAH predicator for sphere.
class SpherePred {
 public:
  SpherePred(const float *vertices)
      : axis_(0), pos_(0.0f), vertices_(vertices) {}

  void Set(int axis, float pos) const {
    axis_ = axis;
    pos_ = pos;
  }

  bool operator()(unsigned int i) const {
    int axis = axis_;
    float pos = pos_;

    nanort::float3 p0(&vertices_[3 * i]);

    float center = p0[axis];

    return (center < pos);
  }

 private:
  mutable int axis_;
  mutable float pos_;
  const float *vertices_;
};

// -----------------------------------------------------

class SphereGeometry {
 public:
  SphereGeometry(const float *vertices, const float *radiuss)
      : vertices_(vertices), radiuss_(radiuss) {}

  /// Compute bounding box for `prim_index`th sphere.
  /// This function is called for each primitive in BVH build.
  void BoundingBox(nanort::float3 *bmin, nanort::float3 *bmax, unsigned int prim_index) const {
    (*bmin)[0] = vertices_[3 * prim_index + 0] - radiuss_[prim_index];
    (*bmin)[1] = vertices_[3 * prim_index + 1] - radiuss_[prim_index];
    (*bmin)[2] = vertices_[3 * prim_index + 2] - radiuss_[prim_index];
    (*bmax)[0] = vertices_[3 * prim_index + 0] + radiuss_[prim_index];
    (*bmax)[1] = vertices_[3 * prim_index + 1] + radiuss_[prim_index];
    (*bmax)[2] = vertices_[3 * prim_index + 2] + radiuss_[prim_index];
  }

  /// Do ray interesection stuff for `prim_index` th primitive and return hit
  /// distance `t`,
  /// varycentric coordinate `u` and `v`.
  /// Returns true if there's intersection.
  bool Intersect(float *t_inout, float *u_out, float *v_out,
                 unsigned int prim_index) const {
    if ((prim_index < trace_options_.prim_ids_range[0]) ||
        (prim_index >= trace_options_.prim_ids_range[1])) {
      return false;
    }

    // http://wiki.cgsociety.org/index.php/Ray_Sphere_Intersection

    const nanort::float3 center(&vertices_[3 * prim_index]);
    const float radius = radiuss_[prim_index];

    nanort::float3 oc = ray_org_ - center;

    float a = vdot(ray_dir_, ray_dir_);
    float b = 2.0 * vdot(ray_dir_, oc);
    float c = vdot(oc, oc) - radius * radius;

    float disc = b * b - 4.0 * a * c;

    float t0, t1;

    if (disc < 0.0) { // no roots
      return false;
    } else if (disc == 0.0) {
      t0 = t1 = -0.5 * (b / a);
    } else {
      // compute q as described above
      float distSqrt = sqrt(disc);
      float q;
      if (b < 0)
        q = (-b - distSqrt) / 2.0;
      else
        q = (-b + distSqrt) / 2.0;

      // compute t0 and t1
      t0 = q / a;
      t1 = c / q;
    }

    // make sure t0 is smaller than t1
    if (t0 > t1) {
      // if t0 is bigger than t1 swap them around
      float temp = t0;
      t0 = t1;
      t1 = temp;
    }
  
    // if t1 is less than zero, the object is in the ray's negative direction
    // and consequently the ray misses the sphere
    if (t1 < 0) {
      return false;
    }

    float t;
    if (t0 < 0) {
      t = t1;
    } else {
      t = t0;
    }

    if (t > (*t_inout)) {
      return false;
    }

    // If you want (u, v), use this.
    // n = normalize(hitP - center)
    // float tu = (atan2(n[0], n[2]) + M_PI) * 0.5 * (1.0 / M_PI);
    // float tv = acos(n[1]) / M_PI;

    (*t_inout) = t;
    (*u_out) = 0.0;
    (*v_out) = 0.0;

    return true;
  }

  /// Prepare BVH traversal(e.g. compute inverse ray direction)
  /// This function is called only once in BVH traversal.
  void PrepareTraversal(const nanort::Ray &ray,
                        const nanort::BVHTraceOptions &trace_options) const {
    ray_org_[0] = ray.org[0];
    ray_org_[1] = ray.org[1];
    ray_org_[2] = ray.org[2];

    ray_dir_[0] = ray.dir[0];
    ray_dir_[1] = ray.dir[1];
    ray_dir_[2] = ray.dir[2];

    trace_options_ = trace_options;
  }

  const float *vertices_;
  const float *radiuss_;
  mutable nanort::float3 ray_org_;
  mutable nanort::float3 ray_dir_;
  mutable nanort::BVHTraceOptions trace_options_;
};

// -----------------------------------------------------

void GenerateRandomSpheres(float* vertices, float* radiuss, size_t n, const float bmin[3], const float bmax[3])
{
  pcg32_state_t rng;
  pcg32_srandom(&rng, 0, 1);

  float bsize = bmax[0] - bmin[0];
  if (bsize < bmax[1] - bmin[1]) {
    bsize = bmax[1] - bmin[1];
  }
  if (bsize < bmax[2] - bmin[2]) {
    bsize = bmax[2] - bmin[2];
  }

  for (size_t i = 0; i < n; i++) {
    // [0, 1)
    float x = pcg32_random(&rng);
    float y = pcg32_random(&rng);
    float z = pcg32_random(&rng);

    vertices[3 * i + 0] = x * (bmax[0] - bmin[0]) + bmin[0];
    vertices[3 * i + 1] = y * (bmax[1] - bmin[1]) + bmin[1];
    vertices[3 * i + 2] = z * (bmax[2] - bmin[2]) + bmin[2];

    // Adjust radius according to # of primitives.
    radiuss[i] = bsize / sqrt((double)n);
  }
}

} // namespace

int main(int argc, char **argv) {
  int width = 512;
  int height = 513;

  if (argc < 2) {
    printf("Needs # of spheres\n");
    return 0;
  }

  size_t n = atoi(argv[1]);

  nanort::BVHBuildOptions options; // Use default option
  options.cache_bbox = false;

  printf("  BVH build option:\n");
  printf("    # of leaf primitives: %d\n", options.min_leaf_primitives);
  printf("    SAH binsize         : %d\n", options.bin_size);

  std::vector<float> vertices(3 * n);
  std::vector<float> radiuss(n);

  float rbmin[3] = {-1, -1, -1};
  float rbmax[3] = { 1,  1,  1};
  GenerateRandomSpheres(&vertices.at(0), &radiuss.at(0), n, rbmin, rbmax); 

  SphereGeometry sphere_geom(&vertices.at(0), &radiuss.at(0));
  SpherePred sphere_pred(&vertices.at(0));
  nanort::BVHAccel<SphereGeometry, SpherePred> accel;
  bool ret = accel.Build(radiuss.size(), options, sphere_geom, sphere_pred);
  assert(ret);

  nanort::BVHBuildStatistics stats = accel.GetStatistics();

  printf("  BVH statistics:\n");
  printf("    # of leaf   nodes: %d\n", stats.num_leaf_nodes);
  printf("    # of branch nodes: %d\n", stats.num_branch_nodes);
  printf("  Max tree depth     : %d\n", stats.max_tree_depth);
  float bmin[3], bmax[3];
  accel.BoundingBox(bmin, bmax);
  printf("  Bmin               : %f, %f, %f\n", bmin[0], bmin[1], bmin[2]);
  printf("  Bmax               : %f, %f, %f\n", bmax[0], bmax[1], bmax[2]);

  std::vector<float> rgb(width * height * 3, 0.0f);

  // Shoot rays.
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {

      // Simple camera. change eye pos and direction fit to .obj model. 

      nanort::Ray ray;
      ray.org[0] = 0.0f;
      ray.org[1] = 0.0f;
      ray.org[2] = 4.0f;

      nanort::float3 dir;
      dir[0] = (x / (float)width) - 0.5f;
      dir[1] = (y / (float)height) - 0.5f;
      dir[2] = -1.0f;
      dir = vnormalize(dir);
      ray.dir[0] = dir[0];
      ray.dir[1] = dir[1];
      ray.dir[2] = dir[2];

      float kFar = 1.0e+30f;
      ray.min_t = 0.0f;
      ray.max_t = kFar;

      nanort::Intersection isect;
      nanort::BVHTraceOptions trace_options;
      bool hit = accel.Traverse(&isect, ray, trace_options, sphere_geom);
      if (hit) {
        // Write your shader here.
        nanort::float3 P;
        P[0] = ray.org[0] + isect.t * ray.dir[0]; 
        P[1] = ray.org[1] + isect.t * ray.dir[1]; 
        P[2] = ray.org[2] + isect.t * ray.dir[2]; 
        unsigned int pid = isect.prim_id;
        nanort::float3 sphere_center(&vertices[3*pid]);
        nanort::float3 normal = vnormalize(P - sphere_center);

        // Flip Y
        rgb[3 * ((height - y - 1) * width + x) + 0] = fabsf(normal[0]);
        rgb[3 * ((height - y - 1) * width + x) + 1] = fabsf(normal[1]);
        rgb[3 * ((height - y - 1) * width + x) + 2] = fabsf(normal[2]);
      }

    }
  }


  SaveImagePNG("render.png", &rgb.at(0), width, height);

  return 0;
}
