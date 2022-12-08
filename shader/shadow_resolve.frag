#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#if defined(RAY_QUERY)
#extension GL_EXT_ray_query : enable
#endif

#if defined(RAY_QUERY_AT)
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_flags_primitive_culling : enable
#endif

#include "common.glsl"
#include "scene.glsl"
#include "lighting/shadow_sampling.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 0) in  vec2 UV;

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };

layout(binding = 1) uniform sampler2D diffuse;
layout(binding = 2) uniform sampler2D normals;
layout(binding = 3) uniform sampler2D depth;

#if defined(SHADOW_MAP)
layout(binding = 4) uniform sampler2D textureSm0;
layout(binding = 5) uniform sampler2D textureSm1;
#endif

#if defined(RAY_QUERY)
layout(binding = 6) uniform accelerationStructureEXT topLevelAS;
#endif

#if defined(RAY_QUERY_AT)
layout(binding  = 7) uniform sampler2D textures[];
layout(binding  = 8,  std430) readonly buffer Vbo { float vert[];   } vbo[];
layout(binding  = 9,  std430) readonly buffer Ibo { uint  index[];  } ibo[];
layout(binding  = 10, std430) readonly buffer Off { uint  offset[]; } iboOff;

vec2 pullTexcoord(uint id, uint vboOffset) {
  float u = vbo[nonuniformEXT(id)].vert[vboOffset*9 + 6];
  float v = vbo[nonuniformEXT(id)].vert[vboOffset*9 + 7];
  return vec2(u,v);
  }

uvec3 pullTrinagleIds(uint id, uint primitiveID) {
  uvec3 index;
  index.x = ibo[nonuniformEXT(id)].index[primitiveID*3+0];
  index.y = ibo[nonuniformEXT(id)].index[primitiveID*3+1];
  index.z = ibo[nonuniformEXT(id)].index[primitiveID*3+2];
  return index;
  }

bool alphaTest(in rayQueryEXT rayQuery, uint id) {
  const bool commited   = false;

  if(id==0)
    return true; // landscape

  const uint  primOffset  = iboOff.offset[id];
  const uint  primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, commited) + primOffset;
  const uvec3 index       = pullTrinagleIds(id,primitiveID);

  const vec2  uv0         = pullTexcoord(id,index.x);
  const vec2  uv1         = pullTexcoord(id,index.y);
  const vec2  uv2         = pullTexcoord(id,index.z);

  vec3 b = vec3(0,rayQueryGetIntersectionBarycentricsEXT(rayQuery,commited));
  b.x = (1-b.y-b.z);
  vec2 uv = (b.x*uv0 + b.y*uv1 + b.z*uv2);

  vec4 d = textureLod(textures[nonuniformEXT(id)],uv,0);
  return (d.a>0.5);
  }
#endif

float lambert(vec3 normal) {
  return max(0.0, dot(scene.sunDir,normal));
  }

vec4 worldPos(ivec2 frag, float depth) {
  const vec2 fragCoord = (frag.xy*scene.screenResInv)*2.0 - vec2(1.0);
  const vec4 scr       = vec4(fragCoord.x, fragCoord.y, depth, 1.0);
  return scene.viewProjectInv * scr;
  }

#if defined(SHADOW_MAP)
float calcShadow(vec4 pos4) {
  return calcShadow(pos4, scene, textureSm0, textureSm1);
  }
#else
float calcShadow(vec4 pos4) { return 1.0; }
#endif

#if defined(RAY_QUERY)
bool rayTest(vec3 pos, float tMin, float tMax) {
  vec3  rayDirection = scene.sunDir;

  uint flags = gl_RayFlagsOpaqueEXT;
#if defined(RAY_QUERY_AT)
  flags |= gl_RayFlagsNoOpaqueEXT;
  flags |= gl_RayFlagsCullFrontFacingTrianglesEXT;
#endif

  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery, topLevelAS, flags, 0xFF,
                        pos, tMin, rayDirection, tMax);

  while(rayQueryProceedEXT(rayQuery)) {
#if defined(RAY_QUERY_AT)
    const uint type = rayQueryGetIntersectionTypeEXT(rayQuery,false);
    const uint id   = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery,false);
    if(type==gl_RayQueryCandidateIntersectionTriangleEXT) {
      const bool opaqueHit = alphaTest(rayQuery,id);
      if(opaqueHit)
        rayQueryConfirmIntersectionEXT(rayQuery);
      }
#endif
    }

  if(rayQueryGetIntersectionTypeEXT(rayQuery, true)==gl_RayQueryCommittedIntersectionNoneEXT)
    return false;
  return true;
  }

float calcRayShadow(vec4 pos4, vec3 normal, float depth) {
  vec4 sp = scene.viewShadow[1]*pos4;
  if(all(lessThan(abs(sp.xy),vec2(abs(sp.w)))))
    return 1.0;
  float dist = linearDepth(depth, scene.clipInfo);
  vec3  p    = pos4.xyz/pos4.w;// - scene.sunDir*20;

  float tMin = 50; // bias?
  if(!rayTest(p,tMin,10000))
    return 1.0;
  return 0.0;
  }
#endif

vec3 flatNormal(vec4 pos4) {
  vec3 pos = pos4.xyz/pos4.w;
  vec3 dx  = dFdx(pos);
  vec3 dy  = dFdy(pos);
  return (cross(dx,dy));
  }

void main(void) {
  const ivec2 fragCoord = ivec2(gl_FragCoord.xy);
  const float d         = texelFetch(depth, fragCoord, 0).r;
  if(d==1.0) {
    outColor = vec4(0);
    return;
    }

  const vec4  diff   = texelFetch(diffuse, fragCoord, 0);
  const vec4  nrm    = texelFetch(normals, fragCoord, 0);
  const vec3  normal = normalize(nrm.xyz*2.0-vec3(1.0));

  const float light  = (diff.a>0) ? 0 : lambert(normal);
  //const vec3  fnorm  = flatNormal(worldPos(fragCoord, d));

  float shadow = 1;
  if(light>0) {
    if(dot(scene.sunDir,normal)<=0)
      shadow = 0;

    const vec4 wpos = worldPos(fragCoord, d);
    shadow = calcShadow(wpos);
#if defined(RAY_QUERY)
    if(shadow>0.01)
      shadow *= calcRayShadow(wpos,normal,d);
#endif
    }

  const vec3  lcolor = scene.sunCl.rgb*light*shadow + scene.ambient;

  outColor = vec4(diff.rgb*lcolor, diff.a);
  // outColor = vec4(vec3(lcolor), diff.a);
  }
