#include "pfxbucket.h"
#include "trlobjects.h"

#include <Tempest/VertexBuffer>

#include "graphics/objectsbucket.h"
#include "graphics/worldview.h"
#include "graphics/pfx/pfxobjects.h"

#include "world/world.h"
#include "particlefx.h"

using namespace Tempest;

struct TrlObjects::Point {
  Vec3     at;
  uint64_t time = 0;
  };

struct TrlObjects::Trail {
  AllocState         st = S_Free;

  Vec3               pos;
  std::vector<Point> pt;

  void               tick(uint64_t dt, Bucket& bucket);
  };

struct TrlObjects::Bucket {
  using PfxState = PfxBucket::PfxState;

  Bucket(const ParticleFx& decl, TrlObjects& /*owner*/, VisualObjects& visual) : decl(decl) {
    maxTime = uint64_t(decl.trlFadeSpeed*1000.f);

    Material mat = decl.visMaterial;
    mat.tex = decl.trlTexture;

    item = visual.get(mat);
    item.setObjMatrix(Matrix4x4::mkIdentity());
    }

  bool isEmpty() const {
    for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
      if(pfxGpu[i].byteSize()>0)
        return false;
      }
    return obj.size()==0;
    }

  size_t alloc() {
    for(size_t i=0; i<obj.size(); ++i) {
      if(obj[i].st==S_Free) {
        obj[i].st = S_Alloc;
        return i;
        }
      }
    obj.emplace_back();
    obj.back().st = S_Alloc;
    return obj.size()-1;
    }

  void tick(uint64_t dt) {
    for(auto& i:obj)
      i.tick(dt,*this);

    while(obj.size()) {
      if(obj.back().st!=S_Free)
        break;
      obj.pop_back();
      }
    }

  void buildVbo(const Vec3& viewDir) {
    size_t cnt = 0;
    for(auto& tr:obj) {
      if(tr.pt.size()<2)
        continue;
      cnt += tr.pt.size()-1;
      }
    vboCpu.resize(6*cnt);
    pfxCpu.resize(cnt);

    size_t i = 0;
    for(auto& tr:obj) {
      if(tr.pt.size()<2)
        continue;

      float maxT = float(std::min(maxTime,tr.pt[0].time));
      for(size_t r=1; r<tr.pt.size(); ++r, ++i) {
        mkSegment(pfxCpu[i],viewDir,tr.pt[r-1],tr.pt[r],maxT);
        }
      }
    }

  void mkSegment(Vertex* v, const Vec3& viewDir, const Point& a, const Point& b, float maxT) {
    auto dp = b.at  - a.at;
    auto n  = -Vec3::crossProduct(viewDir,dp);
    n = n/n.length();
    n = n*decl.trlWidth*2.f;

    float dx[6] = {1,  1, -1,  1, -1, -1};
    float dy[6] = {0,  1,  0,  1,  1,  0};

    float    tA  = 1.f - float(a.time)/maxT;
    float    tB  = 1.f - float(b.time)/maxT;

    uint32_t clA = mkColor(tA);
    uint32_t clB = mkColor(tB);

    for(size_t i=0; i<6; ++i) {
      v[i].pos[0]  = a.at.x + dx[i]*n.x + dy[i]*dp.x;
      v[i].pos[1]  = a.at.y + dx[i]*n.y + dy[i]*dp.y;
      v[i].pos[2]  = a.at.z + dx[i]*n.z + dy[i]*dp.z;

      v[i].norm[0] = 0;
      v[i].norm[1] = 1;
      v[i].norm[2] = 0;

      v[i].uv[0]   = dx[i]*0.5f+0.5f;
      v[i].uv[1]   = 1.f-(tA + dy[i]*(tB-tA));

      v[i].color   = dy[i]==0 ? clA : clB;
      }
    }

  void mkSegment(PfxState& v, const Vec3& viewDir, const Point& a, const Point& b, float maxT) {
    float    tA  = 1.f - float(a.time)/maxT;
    float    tB  = 1.f - float(b.time)/maxT;

    uint32_t clA = mkColor(tA);
    uint32_t clB = mkColor(tB);

    v.pos    = a.at;
    v.color  = clA;
    v.size   = Vec3(decl.trlWidth*2.f,tA,tB);
    v.bits0  = uint32_t(1) << 3;
    v.dir    = b.at  - a.at;
    v.colorB = clB;
    }

  uint32_t mkColor(float clA) const {
    struct Color {
      uint8_t r=255;
      uint8_t g=255;
      uint8_t b=255;
      uint8_t a=255;
      } color;

    clA = std::max(0.f, std::min(clA, 1.f));

    if(decl.visMaterial.alpha==Material::AlphaFunc::AdditiveLight) {
      color.r = uint8_t(255*clA);
      color.g = uint8_t(255*clA);
      color.b = uint8_t(255*clA);
      color.a = uint8_t(255);
      }
    else if(decl.visMaterial.alpha==Material::AlphaFunc::Transparent) {
      color.r = 255;
      color.g = 255;
      color.b = 255;
      color.a = uint8_t(clA*255);
      }
    uint32_t cl;
    std::memcpy(&cl,&color,sizeof(cl));
    return cl;
    }

  const ParticleFx&           decl;
  ObjectsBucket::Item         item;
  Tempest::VertexBuffer<Vertex> vboGpu[Resources::MaxFramesInFlight];
  std::vector<Vertex>         vboCpu;

  Tempest::StorageBuffer      pfxGpu[Resources::MaxFramesInFlight];
  std::vector<PfxState>       pfxCpu;

  std::vector<Trail>          obj;
  uint64_t                    maxTime = 0;
  };


TrlObjects::Item::Item(World& world, const ParticleFx& ow)
  :Item(world.view()->pfxGroup.trails, ow) {
  }

TrlObjects::Item::Item(TrlObjects& trl, const ParticleFx& ow) {
  if(ow.trlTexture==nullptr || ow.trlWidth<=0 || ow.trlFadeSpeed<=0)
    return;

  bucket = &trl.getBucket(ow);
  id     = bucket->alloc();
  }

TrlObjects::Item::Item(TrlObjects::Item&& oth)
  :bucket(oth.bucket), id(oth.id) {
  oth.bucket = nullptr;
  }

TrlObjects::Item& TrlObjects::Item::operator =(TrlObjects::Item&& oth) {
  std::swap(bucket, oth.bucket);
  std::swap(id,     oth.id);
  return *this;
  }

TrlObjects::Item::~Item() {
  if(bucket!=nullptr)
    bucket->obj[id].st = S_Fade;
  }

void TrlObjects::Item::setPosition(const Vec3& pos) {
  if(bucket!=nullptr)
    bucket->obj[id].pos = pos;
  }


void TrlObjects::Trail::tick(uint64_t dt, Bucket& bucket) {
  if(st==S_Free)
    return;

  for(auto& i:pt)
    i.time+=dt;

  if(st!=S_Fade) {
    Point x;
    x.at = pos;
    if(pt.size()==0) {
      pt.push_back(x);
      }
    else if(pt.back().at!=x.at) {
      bool extrude = false;
      if(true && pt.size()>1) {
        auto u = x.at         - pt[pt.size()-2].at;
        auto v = pt.back().at - pt[pt.size()-2].at;
        if(std::abs(Vec3::dotProduct(u,v)-u.length()*v.length()) < 0.001f)
          extrude = true;
        }
      if(extrude)
        pt.back() = x; else
        pt.push_back(x);
      }
    else {
      pt.back().time = 0;
      }
    }

  for(size_t rm=0; rm<=pt.size(); ++rm) {
    if(rm==pt.size() || pt[rm].time<bucket.maxTime) {
      pt.erase(pt.begin(),pt.begin()+int(rm));
      break;
      }
    }

  if(st==S_Fade && pt.size()<=1) {
    st = S_Free;
    pt.clear();
    }
  }

TrlObjects::TrlObjects(VisualObjects& visual)
  : visual(visual){
  }

TrlObjects::~TrlObjects() {
  }

void TrlObjects::tick(uint64_t dt) {
  for(auto& i:bucket)
    i.tick(dt);
  for(auto i=bucket.begin(), e = bucket.end(); i!=e; ) {
    if((*i).isEmpty()) {
      i = bucket.erase(i);
      } else {
      ++i;
      }
    }
  }

void TrlObjects::buildVbo(const Vec3& viewDir) {
  for(auto& i:bucket)
    i.buildVbo(viewDir);
  }

void TrlObjects::preFrameUpdate(uint8_t fId) {
  auto& device = Resources::device();
  for(auto& i:bucket) {
    auto& ssbo = i.pfxGpu[fId];
    if(i.pfxCpu.size()*sizeof(i.pfxCpu[0])!=ssbo.byteSize()) {
      ssbo = device.vbo(BufferHeap::Upload,i.pfxCpu);
      i.item.setPfxData(&ssbo,fId);
      } else {
      ssbo.update(i.pfxCpu);
      }
    }
  }

TrlObjects::Bucket& TrlObjects::getBucket(const ParticleFx &ow) {
  for(auto& i:bucket)
    if(&i.decl==&ow)
      return i;
  bucket.emplace_back(ow,*this,visual);
  return bucket.back();
  }
