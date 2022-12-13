#include "pfxbucket.h"

#include "graphics/mesh/submesh/pfxemittermesh.h"
#include "pfxobjects.h"
#include "particlefx.h"

#include "world/objects/npc.h"

using namespace Tempest;

static uint64_t ppsDiff(const ParticleFx& decl, bool loop, uint64_t time0, uint64_t time1) {
  if(time1<=time0)
    return 0;
  if(decl.prefferedTime==0 && time0==0 && !loop)
    return uint64_t(decl.ppsValue*1000.f);
  if(!loop) {
    time0 = std::min(decl.prefferedTime,time0);
    time1 = std::min(decl.prefferedTime,time1);
    }
  const float pps     = decl.ppsScale(time1)*decl.ppsValue;
  uint64_t    emitted0 = uint64_t(pps*float(time0)/1000.f);
  uint64_t    emitted1 = uint64_t(pps*float(time1)/1000.f);
  return emitted1-emitted0;
  }

float PfxBucket::ParState::lifeTime() const {
  return 1.f-life/float(maxLife);
  }

std::mt19937 PfxBucket::rndEngine;

PfxBucket::PfxBucket(const ParticleFx &decl, PfxObjects& parent, VisualObjects& visual)
  :decl(decl), parent(parent), visual(visual), vertexCount(decl.visTexIsQuadPoly ? 6 : 3) {
  item = visual.get(decl.visMaterial);

  Matrix4x4 ident;
  ident.identity();
  item.setObjMatrix(ident);

  uint64_t lt      = decl.maxLifetime();
  uint64_t pps     = uint64_t(std::ceil(decl.maxPps()));
  uint64_t reserve = (lt*pps+1000-1)/1000;
  blockSize        = size_t(reserve);
  if(blockSize==0)
    blockSize=1;
  }

PfxBucket::~PfxBucket() {
  }

bool PfxBucket::isEmpty() const {
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    if(pfxGpu[i].byteSize()>0)
      return false;
    }
  return impl.size()==0;
  }

size_t PfxBucket::allocBlock() {
  for(size_t i=0;i<block.size();++i) {
    if(!block[i].allocated) {
      block[i].allocated = true;
      block[i].timeTotal = 0;
      return i;
      }
    }

  block.emplace_back();

  Block& b = block.back();
  b.allocated = true;
  b.offset    = particles.size();
  b.timeTotal = 0;

  particles.resize(particles.size()+blockSize);
  pfxCpu   .resize(particles.size());

  for(size_t i=0; i<blockSize; ++i)
    particles[b.offset+i].life = 0;
  return block.size()-1;
  }

void PfxBucket::freeBlock(size_t& i) {
  if(i==size_t(-1))
    return;
  auto& b = block[i];
  assert(b.count==0);

  pfxCpu[b.offset].size = Vec3();

  b.allocated = false;
  i = size_t(-1);
  }

PfxBucket::Block& PfxBucket::getBlock(ImplEmitter &e) {
  if(e.block==size_t(-1)) {
    e.block = allocBlock();
    auto& p = block[e.block];
    p.pos   = e.pos;
    }
  return block[e.block];
  }

PfxBucket::Block& PfxBucket::getBlock(PfxEmitter& e) {
  return getBlock(impl[e.id]);
  }

size_t PfxBucket::allocEmitter() {
  for(size_t i=0; i<impl.size(); ++i) {
    auto& b = impl[i];
    if(b.st==S_Free) {
      b.st = S_Inactive;
      return i;
      }
    }
  impl.emplace_back();
  auto& e = impl.back();
  e.block = size_t(-1); // no backup memory
  e.st    = S_Inactive;

  return impl.size()-1;
  }

void PfxBucket::freeEmitter(size_t& id) {
  auto& v = impl[id];
  if(v.block!=size_t(-1)) {
    auto& b = getBlock(v);
    v.st    = b.count==0 ? S_Free : S_Fade;
    if(b.count==0)
      freeBlock(v.block);
    } else {
    v.st    = S_Free;
    }
  v.next.reset();
  id = size_t(-1);
  shrink();
  }

bool PfxBucket::shrink() {
  while(impl.size()>0) {
    auto& b = impl.back();
    if(b.st!=S_Free)
      break;
    impl.pop_back();
    }
  while(block.size()>0) {
    auto& b = block.back();
    if(b.allocated)
      break;
    block.pop_back();
    }
  if(particles.size()!=block.size()*blockSize) {
    particles.resize(block.size()*blockSize);
    pfxCpu   .resize(particles.size());
    return true;
    }
  return false;
  }

float PfxBucket::randf() {
  return float(rndEngine()%10000)/10000.f;
  }

float PfxBucket::randf(float base, float var) {
  return (2.f*randf()-1.f)*var + base;
  }

void PfxBucket::init(PfxBucket::Block& block, ImplEmitter& emitter, size_t particle) {
  auto& p   = particles[particle];

  p.life    = uint16_t(randf(decl.lspPartAvg,decl.lspPartVar));
  p.maxLife = p.life;

  // TODO: pfx.shpDistribType, pfx.shpDistribWalkSpeed;
  switch(decl.shpType) {
    case ParticleFx::EmitterType::Point:{
      p.pos = Vec3();
      break;
      }
    case ParticleFx::EmitterType::Line:{
      float at = randf();
      p.pos = Vec3(at,at,at);
      break;
      }
    case ParticleFx::EmitterType::Box:{
      if(decl.shpIsVolume) {
        p.pos = Vec3(randf()*2.f-1.f,
                     randf()*2.f-1.f,
                     randf()*2.f-1.f);
        p.pos*=0.5;
        } else {
        // TODO
        p.pos = Vec3(randf()*2.f-1.f,
                     randf()*2.f-1.f,
                     randf()*2.f-1.f);
        p.pos*=0.5;
        }
      break;
      }
    case ParticleFx::EmitterType::Sphere:{
      float theta = float(2.0*M_PI)*randf();
      float phi   = std::acos(1.f - 2.f * randf());
      p.pos = Vec3(std::sin(phi) * std::cos(theta),
                   std::sin(phi) * std::sin(theta),
                   std::cos(phi));
      //p.pos*=0.5;
      if(decl.shpIsVolume)
        p.pos*=randf();
      break;
      }
    case ParticleFx::EmitterType::Circle:{
      float a = float(2.0*M_PI)*randf();
      p.pos = Vec3(std::sin(a),
                   0,
                   std::cos(a));
      //p.pos*=0.5;
      if(decl.shpIsVolume)
        p.pos = p.pos*std::sqrt(randf());
      break;
      }
    case ParticleFx::EmitterType::Mesh:{
      p.pos = Vec3();
      auto mesh = (emitter.mesh!=nullptr) ? emitter.mesh : decl.shpMesh;
      auto pose = (emitter.mesh!=nullptr) ? emitter.pose : nullptr;
      if(mesh!=nullptr) {
        auto pos = mesh->randCoord(randf(),pose);
        pos -= emitter.pos;
        p.pos = emitter.direction[0]*pos.x +
                emitter.direction[1]*pos.y +
                emitter.direction[2]*pos.z;
        }
      break;
      }
    }

  if(decl.shpType!=ParticleFx::EmitterType::Point &&
     decl.shpType!=ParticleFx::EmitterType::Mesh) {
    Vec3 dim = decl.shpDim*decl.shpScale(block.timeTotal);
    p.pos.x*=dim.x;
    p.pos.y*=dim.y;
    p.pos.z*=dim.z;
    }

  switch(decl.shpFOR) {
    case ParticleFx::Frame::Object:
    case ParticleFx::Frame::Node: {
      p.pos += emitter.direction[0]*decl.shpOffsetVec.x +
               emitter.direction[1]*decl.shpOffsetVec.y +
               emitter.direction[2]*decl.shpOffsetVec.z;
      break;
      }
    case ParticleFx::Frame::World: {
      p.pos += decl.shpOffsetVec;
      break;
      }
    }

  switch(decl.dirMode) {
    case ParticleFx::Dir::Rand: {
      float dy    = 1.f - 2.f * randf();
      float sn    = std::sqrt(1-dy*dy);
      float theta = float(2.0*M_PI)*randf();
      float dx    = sn * std::cos(theta);
      float dz    = sn * std::sin(theta);

      p.dir       = Vec3(dx,dy,dz);
      break;
      }
    case ParticleFx::Dir::Dir: {
      float dirAngleHeadVar = decl.dirAngleHeadVar;
      float dirAngleElevVar = decl.dirAngleElevVar;

      // HACK: STARGATE_PARTICLES
      if(decl.dirAngleHeadVar>=180)
        dirAngleHeadVar = 0;
      if(decl.dirAngleElevVar>=180 )
        dirAngleElevVar = 0;

      float head = (90+randf(decl.dirAngleHead,dirAngleHeadVar))*float(M_PI)/180.f;
      float elev = (   randf(decl.dirAngleElev,dirAngleElevVar))*float(M_PI)/180.f;

      float dx = std::cos(elev) * std::cos(head);
      float dy = std::sin(elev);
      float dz = std::cos(elev) * std::sin(head);

      switch(decl.dirFOR) {
        case ParticleFx::Frame::Object:
        case ParticleFx::Frame::Node: {
          p.dir = emitter.direction[0]*dx +
                  emitter.direction[1]*dy +
                  emitter.direction[2]*dz;
          break;
          }
        case ParticleFx::Frame::World: {
          p.dir = Vec3(dx,dy,dz);
          break;
          }
        }
      break;
      }
    case ParticleFx::Dir::Target:
      Vec3 targetPos = emitter.pos;
      switch(decl.dirModeTargetFOR) {
        case ParticleFx::Frame::World:
          targetPos = decl.dirModeTargetPos;
          break;
        case ParticleFx::Frame::Node:
        case ParticleFx::Frame::Object: {
          if(emitter.targetNpc!=nullptr) {
            // MFX_WINDFIST_CAST
            auto mt = emitter.targetNpc->transform();
            mt.project(targetPos);
            } else {
            // IRRLICHT_DIE
            targetPos = emitter.pos;
            }
          break;
          }
        }
      p.dir += targetPos - (emitter.pos+p.pos);
      break;
    }

  if(!decl.useEmittersFOR)
    p.pos += emitter.pos;

  auto l = p.dir.length();
  if(l!=0.f) {
    float velocity = randf(decl.velAvg,decl.velVar);
    p.dir = p.dir*velocity/l;
    }
  }

void PfxBucket::finalize(size_t particle) {
  auto& p = particles[particle];
  p = {};
  pfxCpu[particle] = {};
  }

void PfxBucket::tick(Block& sys, ImplEmitter&, size_t particle, uint64_t dt) {
  ParState& ps = particles[particle+sys.offset];
  if(ps.life==0)
    return;

  if(ps.life<=dt){
    ps.life = 0;
    sys.count--;
    finalize(particle+sys.offset);
    return;
    }

  const float dtF = float(dt);

  // eval particle
  ps.life  = uint16_t(ps.life-dt);
  ps.pos  += ps.dir*dtF;
  ps.dir  += decl.flyGravity*dtF;
  }

void PfxBucket::tick(uint64_t dt, const Vec3& viewPos) {
  if(decl.isDecal()) {
    implTickDecals(dt,viewPos);
    return;
    }
  implTickCommon(dt,viewPos);
  }

void PfxBucket::implTickCommon(uint64_t dt, const Vec3& viewPos) {
  bool doShrink = false;
  for(auto& emitter:impl) {
    if(emitter.st==S_Free)
      continue;

    const auto dp     = emitter.pos-viewPos;
    const bool nearby = (dp.quadLength()<PfxObjects::viewRage*PfxObjects::viewRage);

    if(emitter.next==nullptr && decl.ppsCreateEm!=nullptr && emitter.waitforNext<dt && emitter.st==S_Active) {
      emitter.next.reset(new PfxEmitter(parent,decl.ppsCreateEm));
      auto& e = *emitter.next;
      e.setPosition(emitter.pos.x,emitter.pos.y,emitter.pos.z);
      e.setActive(true);
      e.setLooped(emitter.isLoop);
      }

    if(emitter.waitforNext>=dt)
      emitter.waitforNext-=dt;

    if(emitter.block!=size_t(-1)) {
      auto& p = getBlock(emitter);
      if(p.count>0) {
        for(size_t i=0;i<blockSize;++i)
          tick(p,emitter,i,dt);
        if(p.count==0 && (emitter.st==S_Fade || !nearby)) {
          // free mem
          freeBlock(emitter.block);
          if(emitter.st==S_Fade)
            emitter.st = S_Free;
          doShrink = true;
          continue;
          }
        }
      }

    if(emitter.st==S_Active && nearby) {
      auto& p = getBlock(emitter);
      auto dE = ppsDiff(decl,emitter.isLoop,p.timeTotal,p.timeTotal+dt);
      tickEmit(p,emitter,dE);
      }

    if(emitter.block!=size_t(-1)) {
      auto& p = getBlock(emitter);
      p.timeTotal+=dt;
      }
    }

  if(doShrink)
    shrink();
  }

void PfxBucket::implTickDecals(uint64_t, const Vec3&) {
  for(auto& emitter:impl) {
    if(emitter.st==S_Free)
      continue;

    auto& p = getBlock(emitter);
    if(emitter.st==S_Active || emitter.st==S_Inactive) {
      if(p.count==0)
        tickEmit(p,emitter,1);
      } else
    if(emitter.st==S_Fade) {
      for(size_t i=0; i<blockSize; ++i)
        particles[p.offset+i].life = 0;
      p.count = 0;
      freeBlock(emitter.block);
      emitter.st = S_Free;
      }
    }
  }

void PfxBucket::tickEmit(Block& p, ImplEmitter& emitter, uint64_t emited) {
  size_t lastI = 0;
  for(size_t id=1; emited>0; ++id) {
    const size_t i  = id%blockSize;
    ParState&    ps = particles[i+p.offset];
    if(ps.life==0) { // free slot
      --emited;
      lastI = i;
      init(p,emitter,i+p.offset);
      if(ps.life==0)
        continue;
      p.count++;
      } else {
      // out of slots
      if(lastI==i)
        return;
      }
    }
  }

void PfxBucket::buildSsbo() {
  auto  colorS          = decl.visTexColorStart;
  auto  colorE          = decl.visTexColorEnd;
  auto  visSizeStart    = decl.visSizeStart;
  auto  visSizeEndScale = decl.visSizeEndScale;
  auto  visAlphaStart   = decl.visAlphaStart;
  auto  visAlphaEnd     = decl.visAlphaEnd;
  auto  visAlphaFunc    = decl.visMaterial.alpha;

  for(auto& p:block) {
    if(p.count==0)
      continue;

    for(size_t pId=0; pId<blockSize; ++pId) {
      ParState& ps = particles[pId+p.offset];
      auto&     px = pfxCpu   [pId+p.offset];

      if(ps.life==0) {
        px.size = Vec3();
        continue;
        }

      const float a     = ps.lifeTime();
      const Vec3  cl    = colorS*(1.f-a)        + colorE*a;
      const float clA   = visAlphaStart*(1.f-a) + visAlphaEnd*a;

      const float scale = 1.f*(1.f-a) + a*visSizeEndScale;
      const float szX   = visSizeStart.x*scale;
      const float szY   = visSizeStart.y*scale;
      const float szZ   = 0.1f*((szX+szY)*0.5f);

      struct Color {
        uint8_t r=255;
        uint8_t g=255;
        uint8_t b=255;
        uint8_t a=255;
        } color;

      if(visAlphaFunc==Material::AlphaFunc::AdditiveLight) {
        color.r = uint8_t(cl.x*clA);
        color.g = uint8_t(cl.y*clA);
        color.b = uint8_t(cl.z*clA);
        color.a = uint8_t(255);
        } else {
        color.r = uint8_t(cl.x);
        color.g = uint8_t(cl.y);
        color.b = uint8_t(cl.z);
        color.a = uint8_t(clA*255);
        }
      uint32_t colorU32;
      std::memcpy(&colorU32,&color,4);
      buildBilboard(px,p,ps, colorU32, szX,szY,szZ);
      }
    }
  }

void PfxBucket::buildBilboard(PfxState& v, const Block& p, const ParState& ps, const uint32_t color,
                              float szX, float szY, float szZ) {
  if(decl.useEmittersFOR)
    v.pos = ps.pos + p.pos; else
    v.pos = ps.pos;

  v.size  = Vec3(szX,szY,szZ);
  v.color = color;
  v.bits0 = 0;
  v.bits0 |= uint32_t(decl.visZBias ? 1 : 0);
  v.bits0 |= uint32_t(decl.visTexIsQuadPoly ? 1 : 0) << 1;
  v.bits0 |= uint32_t(decl.visYawAlign ? 1 : 0) << 2;
  v.bits0 |= uint32_t(0) << 3; // TODO: trails
  v.bits0 |= uint32_t(decl.visOrientation) << 4;
  v.dir   = ps.dir;
  }
