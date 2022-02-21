#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "sky_common.glsl"

layout(binding  = 0) uniform sampler2D tLUT;
layout(binding  = 1) uniform sampler2D mLUT;

layout(location = 0) in  vec2 inPos;
layout(location = 0) out vec4 outColor;

const int numScatteringSteps = 32;

vec3 raymarchScattering(vec3 pos,
                        vec3 rayDir, vec3 sunDir,
                        float tMax, float numSteps) {
  float cosTheta = dot(rayDir, sunDir);

  float miePhaseValue      = miePhase(cosTheta);
  float rayleighPhaseValue = rayleighPhase(-cosTheta);

  vec3  lum                = vec3(0.0);
  vec3  transmittance      = vec3(1.0);

  for(int i=0; i<numSteps; ++i) {
    float t  = ((float(i) + 0.3)/numSteps)*tMax;
    float dt = tMax/numSteps;

    vec3 newPos = pos + t*rayDir;

    vec3  rayleighScattering;
    vec3  extinction;
    float mieScattering;
    scatteringValues(newPos, rayleighScattering, mieScattering, extinction);

    vec3 sampleTransmittance = exp(-dt*extinction);

    vec3 sunTransmittance = textureLUT(tLUT, newPos, sunDir);
    vec3 psiMS            = textureLUT(mLUT, newPos, sunDir);

    vec3 rayleighInScattering = rayleighScattering*(rayleighPhaseValue*sunTransmittance + psiMS);
    vec3 mieInScattering      = mieScattering*(miePhaseValue*sunTransmittance + psiMS);
    vec3 inScattering         = (rayleighInScattering + mieInScattering);

    // Integrated scattering within path segment.
    vec3 scatteringIntegral = (inScattering - inScattering * sampleTransmittance) / extinction;

    lum += scatteringIntegral*transmittance;

    transmittance *= sampleTransmittance;
    }

  return lum;
  }

void main() {
  vec2  uv           = inPos*vec2(0.5)+vec2(0.5);
  float azimuthAngle = (uv.x - 0.5)*2.0*PI;
  // Non-linear mapping of altitude. See Section 5.3 of the paper.
  float adjV;
  if(uv.y < 0.5) {
    float coord = 1.0 - 2.0*uv.y;
    adjV = -coord*coord;
    } else {
    float coord = uv.y*2.0 - 1.0;
    adjV = coord*coord;
    }

  const vec3  viewPos       = vec3(0.0, RPlanet + push.plPosY, 0.0);
  const float height        = length(viewPos);
  const vec3  up            = viewPos / height;
  const float horizonAngle  = safeacos(sqrt(height*height - RPlanet*RPlanet) / height) - 0.5*PI;
  const float altitudeAngle = adjV*0.5*PI - horizonAngle;

  float cosAltitude = cos(altitudeAngle);
  vec3  rayDir      = vec3(cosAltitude*sin(azimuthAngle), sin(altitudeAngle), -cosAltitude*cos(azimuthAngle));

  float sunAltitude = (0.5*PI) - acos(dot(push.sunDir, up));
  vec3  sunDir      = vec3(0.0, sin(sunAltitude), -cos(sunAltitude));

#if defined(FOG)
  // vec3  pos1        = inverse(vec3(inPos,1));
  // vec3  pos0        = inverse(vec3(inPos,0));
  // float tMax        = distance(pos0,pos1);
  float tMax        = 10000;
#else
  float atmoDist    = rayIntersect(viewPos, rayDir, RAtmos);
  float groundDist  = rayIntersect(viewPos, rayDir, RPlanet);
  float tMax        = (groundDist < 0.0) ? atmoDist : groundDist;
#endif
  vec3  lum         = raymarchScattering(viewPos, rayDir, sunDir, tMax, float(numScatteringSteps));

  outColor = vec4(lum, 1.0);
  }
