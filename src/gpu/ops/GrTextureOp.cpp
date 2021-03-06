/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrTextureOp.h"
#include <new>
#include "GrAppliedClip.h"
#include "GrCaps.h"
#include "GrContext.h"
#include "GrContextPriv.h"
#include "GrDrawOpTest.h"
#include "GrGeometryProcessor.h"
#include "GrMemoryPool.h"
#include "GrMeshDrawOp.h"
#include "GrOpFlushState.h"
#include "GrQuad.h"
#include "GrResourceProvider.h"
#include "GrShaderCaps.h"
#include "GrTexture.h"
#include "GrTexturePriv.h"
#include "GrTextureProxy.h"
#include "SkGr.h"
#include "SkMathPriv.h"
#include "SkMatrixPriv.h"
#include "SkPoint.h"
#include "SkPoint3.h"
#include "SkRectPriv.h"
#include "SkTo.h"
#include "glsl/GrGLSLColorSpaceXformHelper.h"
#include "glsl/GrGLSLFragmentShaderBuilder.h"
#include "glsl/GrGLSLGeometryProcessor.h"
#include "glsl/GrGLSLVarying.h"
#include "glsl/GrGLSLVertexGeoBuilder.h"

namespace {

enum class Domain : bool { kNo = false, kYes = true };

/**
 * Geometry Processor that draws a texture modulated by a vertex color (though, this is meant to be
 * the same value across all vertices of a quad and uses flat interpolation when available). This is
 * used by TextureOp below.
 */
class TextureGeometryProcessor : public GrGeometryProcessor {
public:
    template <typename Pos> struct VertexCommon {
        using Position = Pos;
        Position fPosition;
        GrColor fColor;
        SkPoint fTextureCoords;
    };

    template <typename Pos, Domain D> struct OptionalDomainVertex;
    template <typename Pos>
    struct OptionalDomainVertex<Pos, Domain::kNo> : VertexCommon<Pos> {
        static constexpr Domain kDomain = Domain::kNo;
    };
    template <typename Pos>
    struct OptionalDomainVertex<Pos, Domain::kYes> : VertexCommon<Pos> {
        static constexpr Domain kDomain = Domain::kYes;
        SkRect fTextureDomain;
    };

    template <typename Pos, Domain D, GrAA> struct OptionalAAVertex;
    template <typename Pos, Domain D>
    struct OptionalAAVertex<Pos, D, GrAA::kNo> : OptionalDomainVertex<Pos, D> {
        static constexpr GrAA kAA = GrAA::kNo;
    };
    template <typename Pos, Domain D>
    struct OptionalAAVertex<Pos, D, GrAA::kYes> : OptionalDomainVertex<Pos, D> {
        static constexpr GrAA kAA = GrAA::kYes;
        SkPoint3 fEdges[4];
    };

    template <typename Pos, Domain D, GrAA AA>
    using Vertex = OptionalAAVertex<Pos, D, AA>;

    static sk_sp<GrGeometryProcessor> Make(GrTextureType textureType, GrPixelConfig textureConfig,
                                           const GrSamplerState::Filter filter,
                                           sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                           sk_sp<GrColorSpaceXform> paintColorSpaceXform,
                                           bool coverageAA, bool perspective, Domain domain,
                                           const GrShaderCaps& caps) {
        return sk_sp<TextureGeometryProcessor>(new TextureGeometryProcessor(
                textureType, textureConfig, filter, std::move(textureColorSpaceXform),
                std::move(paintColorSpaceXform), coverageAA, perspective, domain, caps));
    }

    const char* name() const override { return "TextureGeometryProcessor"; }

    void getGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder* b) const override {
        b->add32(GrColorSpaceXform::XformKey(fTextureColorSpaceXform.get()));
        b->add32(GrColorSpaceXform::XformKey(fPaintColorSpaceXform.get()));
        uint32_t x = this->usesCoverageEdgeAA() ? 0 : 1;
        x |= kFloat3_GrVertexAttribType == fPositions.cpuType() ? 0 : 2;
        x |= fDomain.isInitialized() ? 4 : 0;
        b->add32(x);
    }

    GrGLSLPrimitiveProcessor* createGLSLInstance(const GrShaderCaps& caps) const override {
        class GLSLProcessor : public GrGLSLGeometryProcessor {
        public:
            void setData(const GrGLSLProgramDataManager& pdman, const GrPrimitiveProcessor& proc,
                         FPCoordTransformIter&& transformIter) override {
                const auto& textureGP = proc.cast<TextureGeometryProcessor>();
                this->setTransformDataHelper(SkMatrix::I(), pdman, &transformIter);
                fTextureColorSpaceXformHelper.setData(
                        pdman, textureGP.fTextureColorSpaceXform.get());
                fPaintColorSpaceXformHelper.setData(pdman, textureGP.fPaintColorSpaceXform.get());
            }

        private:
            void onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) override {
                using Interpolation = GrGLSLVaryingHandler::Interpolation;
                const auto& textureGP = args.fGP.cast<TextureGeometryProcessor>();
                fTextureColorSpaceXformHelper.emitCode(
                        args.fUniformHandler, textureGP.fTextureColorSpaceXform.get());
                fPaintColorSpaceXformHelper.emitCode(
                        args.fUniformHandler, textureGP.fPaintColorSpaceXform.get(),
                        kVertex_GrShaderFlag);
                if (kFloat2_GrVertexAttribType == textureGP.fPositions.cpuType()) {
                    args.fVaryingHandler->setNoPerspective();
                }
                args.fVaryingHandler->emitAttributes(textureGP);
                gpArgs->fPositionVar = textureGP.fPositions.asShaderVar();

                this->emitTransforms(args.fVertBuilder,
                                     args.fVaryingHandler,
                                     args.fUniformHandler,
                                     textureGP.fTextureCoords.asShaderVar(),
                                     args.fFPCoordTransformHandler);
                if (fPaintColorSpaceXformHelper.isNoop()) {
                    args.fVaryingHandler->addPassThroughAttribute(
                            textureGP.fColors, args.fOutputColor, Interpolation::kCanBeFlat);
                } else {
                    GrGLSLVarying varying(kHalf4_GrSLType);
                    args.fVaryingHandler->addVarying("color", &varying);
                    args.fVertBuilder->codeAppend("half4 color = ");
                    args.fVertBuilder->appendColorGamutXform(textureGP.fColors.name(),
                                                             &fPaintColorSpaceXformHelper);
                    args.fVertBuilder->codeAppend(";");
                    args.fVertBuilder->codeAppendf("%s = half4(color.rgb * color.a, color.a);",
                                                   varying.vsOut());
                    args.fFragBuilder->codeAppendf("%s = %s;", args.fOutputColor, varying.fsIn());
                }
                args.fFragBuilder->codeAppend("float2 texCoord;");
                args.fVaryingHandler->addPassThroughAttribute(textureGP.fTextureCoords, "texCoord");
                if (textureGP.fDomain.isInitialized()) {
                    args.fFragBuilder->codeAppend("float4 domain;");
                    args.fVaryingHandler->addPassThroughAttribute(
                            textureGP.fDomain, "domain",
                            GrGLSLVaryingHandler::Interpolation::kCanBeFlat);
                    args.fFragBuilder->codeAppend(
                            "texCoord = clamp(texCoord, domain.xy, domain.zw);");
                }
                args.fFragBuilder->codeAppendf("%s = ", args.fOutputColor);
                args.fFragBuilder->appendTextureLookupAndModulate(
                        args.fOutputColor, args.fTexSamplers[0], "texCoord", kFloat2_GrSLType,
                        &fTextureColorSpaceXformHelper);
                args.fFragBuilder->codeAppend(";");
                if (textureGP.usesCoverageEdgeAA()) {
                    bool mulByFragCoordW = false;
                    GrGLSLVarying aaDistVarying(kFloat4_GrSLType,
                                                GrGLSLVarying::Scope::kVertToFrag);
                    if (kFloat3_GrVertexAttribType == textureGP.fPositions.cpuType()) {
                        args.fVaryingHandler->addVarying("aaDists", &aaDistVarying);
                        // The distance from edge equation e to homogeneous point p=sk_Position
                        // is e.x*p.x/p.w + e.y*p.y/p.w + e.z. However, we want screen space
                        // interpolation of this distance. We can do this by multiplying the
                        // varying in the VS by p.w and then multiplying by sk_FragCoord.w in
                        // the FS. So we output e.x*p.x + e.y*p.y + e.z * p.w
                        args.fVertBuilder->codeAppendf(
                                R"(%s = float4(dot(aaEdge0, %s), dot(aaEdge1, %s),
                                               dot(aaEdge2, %s), dot(aaEdge3, %s));)",
                                aaDistVarying.vsOut(), textureGP.fPositions.name(),
                                textureGP.fPositions.name(), textureGP.fPositions.name(),
                                textureGP.fPositions.name());
                        mulByFragCoordW = true;
                    } else {
                        args.fVaryingHandler->addVarying("aaDists", &aaDistVarying);
                        args.fVertBuilder->codeAppendf(
                                R"(%s = float4(dot(aaEdge0.xy, %s.xy) + aaEdge0.z,
                                               dot(aaEdge1.xy, %s.xy) + aaEdge1.z,
                                               dot(aaEdge2.xy, %s.xy) + aaEdge2.z,
                                               dot(aaEdge3.xy, %s.xy) + aaEdge3.z);)",
                                aaDistVarying.vsOut(), textureGP.fPositions.name(),
                                textureGP.fPositions.name(), textureGP.fPositions.name(),
                                textureGP.fPositions.name());
                    }
                    args.fFragBuilder->codeAppendf(
                            "float mindist = min(min(%s.x, %s.y), min(%s.z, %s.w));",
                            aaDistVarying.fsIn(), aaDistVarying.fsIn(), aaDistVarying.fsIn(),
                            aaDistVarying.fsIn());
                    if (mulByFragCoordW) {
                        args.fFragBuilder->codeAppend("mindist *= sk_FragCoord.w;");
                    }
                    args.fFragBuilder->codeAppendf("%s = float4(saturate(mindist));",
                                                   args.fOutputCoverage);
                } else {
                    args.fFragBuilder->codeAppendf("%s = float4(1);", args.fOutputCoverage);
                }
            }
            GrGLSLColorSpaceXformHelper fTextureColorSpaceXformHelper;
            GrGLSLColorSpaceXformHelper fPaintColorSpaceXformHelper;
        };
        return new GLSLProcessor;
    }

    bool usesCoverageEdgeAA() const { return SkToBool(fAAEdges[0].isInitialized()); }

private:
    TextureGeometryProcessor(GrTextureType textureType, GrPixelConfig textureConfig,
                             GrSamplerState::Filter filter,
                             sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                             sk_sp<GrColorSpaceXform> paintColorSpaceXform, bool coverageAA,
                             bool perspective, Domain domain, const GrShaderCaps& caps)
            : INHERITED(kTextureGeometryProcessor_ClassID)
            , fTextureColorSpaceXform(std::move(textureColorSpaceXform))
            , fPaintColorSpaceXform(std::move(paintColorSpaceXform))
            , fSampler(textureType, textureConfig, filter) {
        this->setTextureSamplerCnt(1);

        if (perspective) {
            fPositions = {"position", kFloat3_GrVertexAttribType, kFloat3_GrSLType};
        } else {
            fPositions = {"position", kFloat2_GrVertexAttribType, kFloat2_GrSLType};
        }
        fColors = {"color", kUByte4_norm_GrVertexAttribType, kHalf4_GrSLType};
        fTextureCoords = {"textureCoords", kFloat2_GrVertexAttribType, kFloat2_GrSLType};
        int vertexAttributeCnt = 3;

        if (domain == Domain::kYes) {
            fDomain = {"domain", kFloat4_GrVertexAttribType, kFloat4_GrSLType};
            ++vertexAttributeCnt;
        }
        if (coverageAA) {
            fAAEdges[0] = {"aaEdge0", kFloat3_GrVertexAttribType, kFloat3_GrSLType};
            fAAEdges[1] = {"aaEdge1", kFloat3_GrVertexAttribType, kFloat3_GrSLType};
            fAAEdges[2] = {"aaEdge2", kFloat3_GrVertexAttribType, kFloat3_GrSLType};
            fAAEdges[3] = {"aaEdge3", kFloat3_GrVertexAttribType, kFloat3_GrSLType};
            vertexAttributeCnt += 4;
        }
        this->setVertexAttributeCnt(vertexAttributeCnt);
    }

    const Attribute& onVertexAttribute(int i) const override {
        return IthInitializedAttribute(i, fPositions, fColors, fTextureCoords, fDomain, fAAEdges[0],
                                       fAAEdges[1], fAAEdges[2], fAAEdges[3]);
    }

    const TextureSampler& onTextureSampler(int) const override { return fSampler; }

    Attribute fPositions;
    Attribute fColors;
    Attribute fTextureCoords;
    Attribute fDomain;
    Attribute fAAEdges[4];
    sk_sp<GrColorSpaceXform> fTextureColorSpaceXform;
    sk_sp<GrColorSpaceXform> fPaintColorSpaceXform;
    TextureSampler fSampler;

    typedef GrGeometryProcessor INHERITED;
};

// This computes the four edge equations for a quad, then outsets them and optionally computes a new
// quad as the intersection points of the outset edges. 'x' and 'y' contain the original points as
// input and the outset points as output. 'a', 'b', and 'c' are the edge equation coefficients on
// output. If outsetCorners is true then 'u' and 'v' should hold the texture coordinates on input
// and will also be outset.
static void compute_quad_edges_and_outset_vertices(GrQuadAAFlags aaFlags, Sk4f* x, Sk4f* y, Sk4f* a,
                                                   Sk4f* b, Sk4f* c, bool outsetCorners = false,
                                                   Sk4f* u = nullptr, Sk4f* v = nullptr) {
    static constexpr auto fma = SkNx_fma<4, float>;
    // These rotate the points/edge values either clockwise or counterclockwise assuming tri strip
    // order.
    auto nextCW  = [](const Sk4f& v) { return SkNx_shuffle<2, 0, 3, 1>(v); };
    auto nextCCW = [](const Sk4f& v) { return SkNx_shuffle<1, 3, 0, 2>(v); };

    // Compute edge equations for the quad.
    auto xnext = nextCCW(*x);
    auto ynext = nextCCW(*y);
    // xdiff and ydiff will comprise the normalized vectors pointing along each quad edge.
    auto xdiff = xnext - *x;
    auto ydiff = ynext - *y;
    auto invLengths = fma(xdiff, xdiff, ydiff * ydiff).rsqrt();
    xdiff *= invLengths;
    ydiff *= invLengths;

    // Use above vectors to compute edge equations.
    *c = fma(xnext, *y,  -ynext * *x) * invLengths;
    // Make sure the edge equations have their normals facing into the quad in device space.
    auto test = fma(ydiff, nextCW(*x), fma(-xdiff, nextCW(*y), *c));
    if ((test < Sk4f(0)).anyTrue()) {
        *a = -ydiff;
        *b = xdiff;
        *c = -*c;
    } else {
        *a = ydiff;
        *b = -xdiff;
    }
    // Outset the edge equations so aa coverage evaluates to zero half a pixel away from the
    // original quad edge.
    *c += 0.5f;

    if (aaFlags != GrQuadAAFlags::kAll) {
        // This order is the same order the edges appear in xdiff/ydiff and therefore as the
        // edges in a/b/c.
        auto mask = Sk4f(GrQuadAAFlags::kLeft & aaFlags ? 1.f : 0.f,
                         GrQuadAAFlags::kBottom & aaFlags ? 1.f : 0.f,
                         GrQuadAAFlags::kTop & aaFlags ? 1.f : 0.f,
                         GrQuadAAFlags::kRight & aaFlags ? 1.f : 0.f);
        // Outset edge equations for masked out edges another pixel so that they always evaluate
        // >= 1.
        *c += (1.f - mask);
        if (outsetCorners) {
            // Do the vertex outset.
            mask *= 0.5f;
            auto maskCW = nextCW(mask);
            *x += maskCW * -xdiff + mask * nextCW(xdiff);
            *y += maskCW * -ydiff + mask * nextCW(ydiff);
            // We want to extend the texture coords by the same proportion as the positions.
            maskCW *= invLengths;
            mask *= nextCW(invLengths);
            Sk4f udiff = nextCCW(*u) - *u;
            Sk4f vdiff = nextCCW(*v) - *v;
            *u += maskCW * -udiff + mask * nextCW(udiff);
            *v += maskCW * -vdiff + mask * nextCW(vdiff);
        }
    } else if (outsetCorners) {
        *x += 0.5f * (-xdiff + nextCW(xdiff));
        *y += 0.5f * (-ydiff + nextCW(ydiff));
        Sk4f t = 0.5f * invLengths;
        Sk4f udiff = nextCCW(*u) - *u;
        Sk4f vdiff = nextCCW(*v) - *v;
        *u += t * -udiff + nextCW(t) * nextCW(udiff);
        *v += t * -vdiff + nextCW(t) * nextCW(vdiff);
    }
}

namespace {
// This is a class soley so it can be partially specialized (functions cannot be).
template <typename V, GrAA AA = V::kAA, typename Position = typename V::Position>
class VertexAAHandler;

template<typename V> class VertexAAHandler<V, GrAA::kNo, SkPoint> {
public:
    static void AssignPositionsAndTexCoords(V* vertices, const GrPerspQuad& quad,
                                            GrQuadAAFlags aaFlags, const SkRect& texRect) {
        // Should be kNone for non-AA and kAll for MSAA.
        SkASSERT(aaFlags == GrQuadAAFlags::kNone || aaFlags == GrQuadAAFlags::kAll);
        SkASSERT((quad.w4f() == Sk4f(1.f)).allTrue());
        SkPointPriv::SetRectTriStrip(&vertices[0].fTextureCoords, texRect, sizeof(V));
        for (int i = 0; i < 4; ++i) {
            vertices[i].fPosition = {quad.x(i), quad.y(i)};
        }
    }
};

template<typename V> class VertexAAHandler<V, GrAA::kNo, SkPoint3> {
public:
    static void AssignPositionsAndTexCoords(V* vertices, const GrPerspQuad& quad,
                                            GrQuadAAFlags aaFlags, const SkRect& texRect) {
        // Should be kNone for non-AA and kAll for MSAA.
        SkASSERT(aaFlags == GrQuadAAFlags::kNone || aaFlags == GrQuadAAFlags::kAll);
        SkPointPriv::SetRectTriStrip(&vertices[0].fTextureCoords, texRect, sizeof(V));
        for (int i = 0; i < 4; ++i) {
            vertices[i].fPosition = quad.point(i);
        }
    }
};

template<typename V> class VertexAAHandler<V, GrAA::kYes, SkPoint> {
public:
    static void AssignPositionsAndTexCoords(V* vertices, const GrPerspQuad& quad,
                                            GrQuadAAFlags aaFlags, const SkRect& texRect) {
        SkASSERT((quad.w4f() == Sk4f(1.f)).allTrue());
        if (aaFlags == GrQuadAAFlags::kNone) {
            for (int i = 0; i < 4; ++i) {
                vertices[i].fPosition = {quad.x(i), quad.y(i)};
                for (int j = 0; j < 4; ++j) {
                    // This works because the position w components are known to be 1.
                    vertices[i].fEdges[j] = {0, 0, 1};
                }
            }
            SkPointPriv::SetRectTriStrip(&vertices[0].fTextureCoords, texRect, sizeof(V));
            return;
        }
        auto x = quad.x4f();
        auto y = quad.y4f();
        Sk4f a, b, c;
        Sk4f u{texRect.fLeft, texRect.fLeft, texRect.fRight, texRect.fRight};
        Sk4f v{texRect.fTop, texRect.fBottom, texRect.fTop, texRect.fBottom};
        compute_quad_edges_and_outset_vertices(aaFlags, &x, &y, &a, &b, &c, true, &u, &v);

        // Faster to store the Sk4fs all at once rather than element-by-element into vertices.
        float xs[4], ys[4], as[4], bs[4], cs[4], us[4], vs[4];
        x.store(xs);
        y.store(ys);
        a.store(as);
        b.store(bs);
        c.store(cs);
        u.store(us);
        v.store(vs);
        for (int i = 0; i < 4; ++i) {
            vertices[i].fPosition = {xs[i], ys[i]};
            vertices[i].fTextureCoords = {us[i], vs[i]};
            for (int j = 0; j < 4; ++j) {
                vertices[i].fEdges[j]  = {as[j], bs[j], cs[j]};
            }
        }
    }
};

template<typename V> class VertexAAHandler<V, GrAA::kYes, SkPoint3> {
public:
    static void AssignPositionsAndTexCoords(V* vertices, const GrPerspQuad& quad,
                                            GrQuadAAFlags aaFlags, const SkRect& texRect) {
        auto x = quad.x4f();
        auto y = quad.y4f();
        auto iw = quad.iw4f();

        if ((iw == Sk4f(1)).allTrue() && aaFlags == GrQuadAAFlags::kNone) {
            for (int i = 0; i < 4; ++i) {
                vertices[i].fPosition = quad.point(i);
                for (int j = 0; j < 4; ++j) {
                    // This works because the position w components are known to be 1.
                    vertices[i].fEdges[j] = {0, 0, 1};
                }
            }
            SkPointPriv::SetRectTriStrip(&vertices[0].fTextureCoords, texRect, sizeof(V));
            return;
        }
        Sk4f a, b, c;
        auto x2d = x * iw;
        auto y2d = y * iw;
        compute_quad_edges_and_outset_vertices(aaFlags, &x2d, &y2d, &a, &b, &c);
        auto w = quad.w4f();
        static const float kOutset = 0.5f;
        Sk4f u{texRect.fLeft, texRect.fLeft, texRect.fRight, texRect.fRight};
        Sk4f v{texRect.fTop, texRect.fBottom, texRect.fTop, texRect.fBottom};
        if ((GrQuadAAFlags::kLeft | GrQuadAAFlags::kRight) & aaFlags) {
            // For each entry in x the equivalent entry in opX is the left/right opposite and so on.
            Sk4f opX = SkNx_shuffle<2, 3, 0, 1>(x);
            Sk4f opW = SkNx_shuffle<2, 3, 0, 1>(w);
            Sk4f opY = SkNx_shuffle<2, 3, 0, 1>(y);
            // vx/vy holds the device space left-to-right vectors along top and bottom of the quad.
            Sk2f vx = SkNx_shuffle<2, 3>(x2d) - SkNx_shuffle<0, 1>(x2d);
            Sk2f vy = SkNx_shuffle<2, 3>(y2d) - SkNx_shuffle<0, 1>(y2d);
            Sk2f len = SkNx_fma(vx, vx, vy * vy).sqrt();
            // For each device space corner, devP, label its left/right opposite device space point
            // opDevPt. The new device space point is opDevPt + s (devPt - opDevPt) where s is
            // (length(devPt - opDevPt) + 0.5) / length(devPt - opDevPt);
            Sk4f s = SkNx_shuffle<0, 1, 0, 1>((len + kOutset) / len);
            // Compute t in homogeneous space from s using similar triangles so that we can produce
            // homogeneous outset vertices for perspective-correct interpolation.
            Sk4f sOpW = s * opW;
            Sk4f t = sOpW / (sOpW + (1.f - s) * w);
            // mask is used to make the t values be 1 when the left/right side is not antialiased.
            Sk4f mask(GrQuadAAFlags::kLeft & aaFlags  ? 1.f : 0.f,
                      GrQuadAAFlags::kLeft & aaFlags  ? 1.f : 0.f,
                      GrQuadAAFlags::kRight & aaFlags ? 1.f : 0.f,
                      GrQuadAAFlags::kRight & aaFlags ? 1.f : 0.f);
            t = t * mask + (1.f - mask);
            x = opX + t * (x - opX);
            y = opY + t * (y - opY);
            w = opW + t * (w - opW);

            Sk4f opU = SkNx_shuffle<2, 3, 0, 1>(u);
            Sk4f opV = SkNx_shuffle<2, 3, 0, 1>(v);
            u = opU + t * (u - opU);
            v = opV + t * (v - opV);
            if ((GrQuadAAFlags::kTop | GrQuadAAFlags::kBottom) & aaFlags) {
                // Update the 2D points for the top/bottom calculation.
                iw = w.invert();
                x2d = x * iw;
                y2d = y * iw;
            }
        }

        if ((GrQuadAAFlags::kTop | GrQuadAAFlags::kBottom) & aaFlags) {
            // This operates the same as above but for top/bottom rather than left/right.
            Sk4f opX = SkNx_shuffle<1, 0, 3, 2>(x);
            Sk4f opW = SkNx_shuffle<1, 0, 3, 2>(w);
            Sk4f opY = SkNx_shuffle<1, 0, 3, 2>(y);

            Sk2f vx = SkNx_shuffle<1, 3>(x2d) - SkNx_shuffle<0, 2>(x2d);
            Sk2f vy = SkNx_shuffle<1, 3>(y2d) - SkNx_shuffle<0, 2>(y2d);
            Sk2f len = SkNx_fma(vx, vx, vy * vy).sqrt();

            Sk4f s = SkNx_shuffle<0, 0, 1, 1>((len + kOutset) / len);

            Sk4f sOpW = s * opW;
            Sk4f t = sOpW / (sOpW + (1.f - s) * w);

            Sk4f mask(GrQuadAAFlags::kTop    & aaFlags ? 1.f : 0.f,
                      GrQuadAAFlags::kBottom & aaFlags ? 1.f : 0.f,
                      GrQuadAAFlags::kTop    & aaFlags ? 1.f : 0.f,
                      GrQuadAAFlags::kBottom & aaFlags ? 1.f : 0.f);
            t = t * mask + (1.f - mask);
            x = opX + t * (x - opX);
            y = opY + t * (y - opY);
            w = opW + t * (w - opW);

            Sk4f opU = SkNx_shuffle<1, 0, 3, 2>(u);
            Sk4f opV = SkNx_shuffle<1, 0, 3, 2>(v);
            u = opU + t * (u - opU);
            v = opV + t * (v - opV);
        }
        // Faster to store the Sk4fs all at once rather than element-by-element into vertices.
        float xs[4], ys[4], ws[4], as[4], bs[4], cs[4], us[4], vs[4];
        x.store(xs);
        y.store(ys);
        w.store(ws);
        a.store(as);
        b.store(bs);
        c.store(cs);
        u.store(us);
        v.store(vs);
        for (int i = 0; i < 4; ++i) {
            vertices[i].fPosition = {xs[i], ys[i], ws[i]};
            vertices[i].fTextureCoords = {us[i], vs[i]};
            for (int j = 0; j < 4; ++j) {
                vertices[i].fEdges[j] = {as[j], bs[j], cs[j]};
            }
        }
    }
};

template <typename V, Domain D = V::kDomain> struct DomainAssigner;

template <typename V> struct DomainAssigner<V, Domain::kYes> {
    static void Assign(V* vertices, Domain domain, GrSamplerState::Filter filter,
                       const SkRect& srcRect, GrSurfaceOrigin origin, float iw, float ih) {
        static constexpr SkRect kLargeRect = {-2, -2, 2, 2};
        SkRect domainRect;
        if (domain == Domain::kYes) {
            auto ltrb = Sk4f::Load(&srcRect);
            if (filter == GrSamplerState::Filter::kBilerp) {
                auto rblt = SkNx_shuffle<2, 3, 0, 1>(ltrb);
                auto whwh = (rblt - ltrb).abs();
                auto c = (rblt + ltrb) * 0.5f;
                static const Sk4f kOffsets = {0.5f, 0.5f, -0.5f, -0.5f};
                ltrb = (whwh < 1.f).thenElse(c, ltrb + kOffsets);
            }
            ltrb *= Sk4f(iw, ih, iw, ih);
            if (origin == kBottomLeft_GrSurfaceOrigin) {
                static const Sk4f kMul = {1.f, -1.f, 1.f, -1.f};
                static const Sk4f kAdd = {0.f, 1.f, 0.f, 1.f};
                ltrb = SkNx_shuffle<0, 3, 2, 1>(kMul * ltrb + kAdd);
            }
            ltrb.store(&domainRect);
        } else {
            domainRect = kLargeRect;
        }
        for (int i = 0; i < 4; ++i) {
            vertices[i].fTextureDomain = domainRect;
        }
    }
};

template <typename V> struct DomainAssigner<V, Domain::kNo> {
    static void Assign(V*, Domain domain, GrSamplerState::Filter, const SkRect&, GrSurfaceOrigin,
                       float iw, float ih) {
        SkASSERT(domain == Domain::kNo);
    }
};

}  // anonymous namespace

template <typename V>
static void tessellate_quad(const GrPerspQuad& devQuad, GrQuadAAFlags aaFlags,
                            const SkRect& srcRect, GrColor color, GrSurfaceOrigin origin,
                            GrSamplerState::Filter filter, V* vertices, SkScalar iw, SkScalar ih,
                            Domain domain) {
    SkRect texRect = {
            iw * srcRect.fLeft,
            ih * srcRect.fTop,
            iw * srcRect.fRight,
            ih * srcRect.fBottom
    };
    if (origin == kBottomLeft_GrSurfaceOrigin) {
        texRect.fTop = 1.f - texRect.fTop;
        texRect.fBottom = 1.f - texRect.fBottom;
    }
    VertexAAHandler<V>::AssignPositionsAndTexCoords(vertices, devQuad, aaFlags, texRect);
    vertices[0].fColor = color;
    vertices[1].fColor = color;
    vertices[2].fColor = color;
    vertices[3].fColor = color;
    DomainAssigner<V>::Assign(vertices, domain, filter, srcRect, origin, iw, ih);
}

static bool aa_has_effect_for_rect_stays_rect(const GrPerspQuad& quad) {
    SkASSERT((quad.w4f() == Sk4f(1)).allTrue());
    float ql = quad.x(0);
    float qt = quad.y(0);
    float qr = quad.x(3);
    float qb = quad.y(3);
    return !SkScalarIsInt(ql) || !SkScalarIsInt(qr) || !SkScalarIsInt(qt) || !SkScalarIsInt(qb);
}

static bool filter_has_effect_for_rect_stays_rect(const GrPerspQuad& quad, const SkRect& srcRect) {
    SkASSERT((quad.w4f() == Sk4f(1)).allTrue());
    float ql = quad.x(0);
    float qt = quad.y(0);
    float qr = quad.x(3);
    float qb = quad.y(3);
    // Disable filtering when there is no scaling of the src rect and the src rect and dst rect
    // align fractionally. If we allow inverted src rects this logic needs to consider that.
    SkASSERT(srcRect.isSorted());
    return (qr - ql) != srcRect.width() || (qb - qt) != srcRect.height() ||
           SkScalarFraction(ql) != SkScalarFraction(srcRect.fLeft) ||
           SkScalarFraction(qt) != SkScalarFraction(srcRect.fTop);
}

/**
 * Op that implements GrTextureOp::Make. It draws textured quads. Each quad can modulate against a
 * the texture by color. The blend with the destination is always src-over. The edges are non-AA.
 */
class TextureOp final : public GrMeshDrawOp {
public:
    static std::unique_ptr<GrDrawOp> Make(GrContext* context,
                                          sk_sp<GrTextureProxy> proxy,
                                          GrSamplerState::Filter filter,
                                          GrColor color,
                                          const SkRect& srcRect,
                                          const SkRect& dstRect,
                                          GrAAType aaType,
                                          GrQuadAAFlags aaFlags,
                                          SkCanvas::SrcRectConstraint constraint,
                                          const SkMatrix& viewMatrix,
                                          sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                          sk_sp<GrColorSpaceXform> paintColorSpaceXform) {
        GrOpMemoryPool* pool = context->contextPriv().opMemoryPool();

        return pool->allocate<TextureOp>(
                std::move(proxy), filter, color, srcRect, dstRect, aaType, aaFlags, constraint,
                viewMatrix, std::move(textureColorSpaceXform), std::move(paintColorSpaceXform));
    }
    static std::unique_ptr<GrDrawOp> Make(GrContext* context,
                                          const GrRenderTargetContext::TextureSetEntry set[],
                                          int cnt, GrSamplerState::Filter filter, GrColor color,
                                          GrAAType aaType, const SkMatrix& viewMatrix,
                                          sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                          sk_sp<GrColorSpaceXform> paintColorSpaceXform) {
        size_t size = sizeof(TextureOp) + sizeof(Proxy) * (cnt - 1);
        GrOpMemoryPool* pool = context->contextPriv().opMemoryPool();
        void* mem = pool->allocate(size);
        return std::unique_ptr<GrDrawOp>(new (mem) TextureOp(
                set, cnt, filter, color, aaType, viewMatrix, std::move(textureColorSpaceXform),
                std::move(paintColorSpaceXform)));
    }

    ~TextureOp() override {
        for (unsigned p = 0; p < fProxyCnt; ++p) {
            if (fFinalized) {
                fProxies[p].fProxy->completedRead();
            } else {
                fProxies[p].fProxy->unref();
            }
        }
    }

    const char* name() const override { return "TextureOp"; }

    void visitProxies(const VisitProxyFunc& func, VisitorType visitor) const override {
        if (visitor == VisitorType::kAllocatorGather && fCanSkipAllocatorGather) {
            return;
        }
        for (unsigned p = 0; p < fProxyCnt; ++p) {
            func(fProxies[p].fProxy);
        }
    }

    SkString dumpInfo() const override {
        SkString str;
        str.appendf("# draws: %d\n", fQuads.count());
        int q = 0;
        for (unsigned p = 0; p < fProxyCnt; ++p) {
            str.appendf("Proxy ID: %d, Filter: %d\n", fProxies[p].fProxy->uniqueID().asUInt(),
                        static_cast<int>(fFilter));
            for (int i = 0; i < fProxies[p].fQuadCnt; ++i, ++q) {
                const Quad& quad = fQuads[q];
                str.appendf(
                        "%d: Color: 0x%08x, TexRect [L: %.2f, T: %.2f, R: %.2f, B: %.2f] "
                        "Quad [(%.2f, %.2f), (%.2f, %.2f), (%.2f, %.2f), (%.2f, %.2f)]\n",
                        i, quad.color(), quad.srcRect().fLeft, quad.srcRect().fTop,
                        quad.srcRect().fRight, quad.srcRect().fBottom, quad.quad().point(0).fX,
                        quad.quad().point(0).fY, quad.quad().point(1).fX, quad.quad().point(1).fY,
                        quad.quad().point(2).fX, quad.quad().point(2).fY, quad.quad().point(3).fX,
                        quad.quad().point(3).fY);
            }
        }
        str += INHERITED::dumpInfo();
        return str;
    }

    RequiresDstTexture finalize(const GrCaps& caps, const GrAppliedClip* clip) override {
        SkASSERT(!fFinalized);
        fFinalized = true;
        for (unsigned p = 0; p < fProxyCnt; ++p) {
            fProxies[p].fProxy->addPendingRead();
            fProxies[p].fProxy->unref();
        }
        return RequiresDstTexture::kNo;
    }

    FixedFunctionFlags fixedFunctionFlags() const override {
        return this->aaType() == GrAAType::kMSAA ? FixedFunctionFlags::kUsesHWAA
                                                 : FixedFunctionFlags::kNone;
    }

    DEFINE_OP_CLASS_ID

private:
    friend class ::GrOpMemoryPool;

    TextureOp(sk_sp<GrTextureProxy> proxy, GrSamplerState::Filter filter, GrColor color,
              const SkRect& srcRect, const SkRect& dstRect, GrAAType aaType, GrQuadAAFlags aaFlags,
              SkCanvas::SrcRectConstraint constraint, const SkMatrix& viewMatrix,
              sk_sp<GrColorSpaceXform> textureColorSpaceXform,
              sk_sp<GrColorSpaceXform> paintColorSpaceXform)
            : INHERITED(ClassID())
            , fTextureColorSpaceXform(std::move(textureColorSpaceXform))
            , fPaintColorSpaceXform(std::move(paintColorSpaceXform))
            , fFilter(static_cast<unsigned>(filter))
            , fAAType(static_cast<unsigned>(aaType))
            , fFinalized(0) {
        switch (aaType) {
            case GrAAType::kNone:
                aaFlags = GrQuadAAFlags::kNone;
                break;
            case GrAAType::kCoverage:
                if (aaFlags == GrQuadAAFlags::kNone) {
                    fAAType = static_cast<unsigned>(GrAAType::kNone);
                }
                break;
            case GrAAType::kMSAA:
                aaFlags = GrQuadAAFlags::kAll;
                break;
            case GrAAType::kMixedSamples:
                SK_ABORT("Should not use mixed sample AA");
                break;
        }
        fPerspective = static_cast<unsigned>(viewMatrix.hasPerspective());
        auto quad = GrPerspQuad(dstRect, viewMatrix);
        // We expect our caller to have already caught this optimization.
        SkASSERT(!srcRect.contains(proxy->getWorstCaseBoundsRect()) ||
                 constraint == SkCanvas::kFast_SrcRectConstraint);
        if (viewMatrix.rectStaysRect()) {
            if (this->aaType() == GrAAType::kCoverage && !aa_has_effect_for_rect_stays_rect(quad)) {
                fAAType = static_cast<unsigned>(GrAAType::kNone);
                aaFlags = GrQuadAAFlags::kNone;
            }
            if (this->filter() != GrSamplerState::Filter::kNearest &&
                !filter_has_effect_for_rect_stays_rect(quad, srcRect)) {
                fFilter = static_cast<unsigned>(GrSamplerState::Filter::kNearest);
            }
        }
        // We may have had a strict constraint with nearest filter solely due to possible AA bloat.
        // If we don't have (or determined we don't need) coverage AA then we can skip using a
        // domain.
        if (constraint == SkCanvas::kStrict_SrcRectConstraint &&
            this->filter() == GrSamplerState::Filter::kNearest &&
            this->aaType() != GrAAType::kCoverage) {
            constraint = SkCanvas::kFast_SrcRectConstraint;
        }
        const auto& draw = fQuads.emplace_back(srcRect, quad, aaFlags, constraint, color);
        fProxyCnt = 1;
        fProxies[0] = {proxy.release(), 1};
        auto bounds = quad.bounds();
        this->setBounds(bounds, HasAABloat(this->aaType() == GrAAType::kCoverage), IsZeroArea::kNo);
        fDomain = static_cast<unsigned>(draw.domain());
        fCanSkipAllocatorGather =
                static_cast<unsigned>(fProxies[0].fProxy->canSkipResourceAllocator());
    }
    TextureOp(const GrRenderTargetContext::TextureSetEntry set[], int cnt,
              GrSamplerState::Filter filter, GrColor color, GrAAType aaType,
              const SkMatrix& viewMatrix, sk_sp<GrColorSpaceXform> textureColorSpaceXform,
              sk_sp<GrColorSpaceXform> paintColorSpaceXform)
            : INHERITED(ClassID())
            , fTextureColorSpaceXform(std::move(textureColorSpaceXform))
            , fPaintColorSpaceXform(std::move(paintColorSpaceXform))
            , fFilter(static_cast<unsigned>(filter))
            , fAAType(static_cast<unsigned>(aaType))
            , fFinalized(0) {
        fQuads.reserve(cnt);
        fProxyCnt = SkToUInt(cnt);
        SkRect bounds = SkRectPriv::MakeLargestInverted();
        bool needAA = false;
        bool mustFilter = false;
        fCanSkipAllocatorGather = static_cast<unsigned>(true);
        bool rectStaysRect = viewMatrix.rectStaysRect();
        for (unsigned p = 0; p < fProxyCnt; ++p) {
            fProxies[p].fProxy = SkRef(set[p].fProxy.get());
            fProxies[p].fQuadCnt = 1;
            SkASSERT(fProxies[p].fProxy->textureType() == fProxies[0].fProxy->textureType());
            SkASSERT(fProxies[p].fProxy->config() == fProxies[0].fProxy->config());
            if (!fProxies[p].fProxy->canSkipResourceAllocator()) {
                fCanSkipAllocatorGather = static_cast<unsigned>(false);
            }
            auto quad = GrPerspQuad(set[p].fDstRect, viewMatrix);
            bounds.joinPossiblyEmptyRect(quad.bounds());
            GrQuadAAFlags aaFlags = set[p].fAAFlags;
            switch (aaType) {
                case GrAAType::kNone:
                    aaFlags = GrQuadAAFlags::kNone;
                    break;
                case GrAAType::kCoverage:
                    if (rectStaysRect) {
                        if (aaFlags != GrQuadAAFlags::kNone &&
                            !aa_has_effect_for_rect_stays_rect(quad)) {
                            aaFlags = GrQuadAAFlags::kNone;
                        }
                    }
                    break;
                case GrAAType::kMSAA:
                    aaFlags = GrQuadAAFlags::kAll;
                    break;
                case GrAAType::kMixedSamples:
                    SK_ABORT("Should not use mixed sample AA");
                    break;
            }
            needAA = needAA || (set[p].fAAFlags != GrQuadAAFlags::kNone);
            if (!mustFilter && this->filter() != GrSamplerState::Filter::kNearest) {
                mustFilter = !rectStaysRect ||
                             filter_has_effect_for_rect_stays_rect(quad, set[p].fSrcRect);
            }
            fQuads.emplace_back(set[p].fSrcRect, quad, aaFlags, SkCanvas::kFast_SrcRectConstraint,
                                color);
        }
        if (!needAA) {
            fAAType = static_cast<unsigned>(GrAAType::kNone);
        }
        if (!mustFilter) {
            fFilter = static_cast<unsigned>(GrSamplerState::Filter::kNearest);
        }
        this->setBounds(bounds, HasAABloat(this->aaType() == GrAAType::kCoverage), IsZeroArea::kNo);
        fPerspective = static_cast<unsigned>(viewMatrix.hasPerspective());
        fDomain = static_cast<unsigned>(false);
    }

    template <typename Pos, Domain D, GrAA AA>
    void tess(void* v, const GrGeometryProcessor* gp, const GrTextureProxy* proxy, int start,
              int cnt) const {
        TRACE_EVENT0("skia", TRACE_FUNC);
        using Vertex = TextureGeometryProcessor::Vertex<Pos, D, AA>;
        SkASSERT(gp->debugOnly_vertexStride() == sizeof(Vertex));
        auto vertices = static_cast<Vertex*>(v);
        auto origin = proxy->origin();
        const auto* texture = proxy->peekTexture();
        float iw = 1.f / texture->width();
        float ih = 1.f / texture->height();

        for (int i = start; i < start + cnt; ++i) {
            const auto q = fQuads[i];
            tessellate_quad<Vertex>(q.quad(), q.aaFlags(), q.srcRect(), q.color(), origin,
                                    this->filter(), vertices, iw, ih, q.domain());
            vertices += 4;
        }
    }

    void onPrepareDraws(Target* target) override {
        TRACE_EVENT0("skia", TRACE_FUNC);
        bool hasPerspective = false;
        Domain domain = Domain::kNo;
        int numProxies = 0;
        int numTotalQuads = 0;
        auto textureType = fProxies[0].fProxy->textureType();
        auto config = fProxies[0].fProxy->config();
        GrAAType aaType = this->aaType();
        for (const auto& op : ChainRange<TextureOp>(this)) {
            hasPerspective |= op.fPerspective;
            if (op.fDomain) {
                domain = Domain::kYes;
            }
            numProxies += op.fProxyCnt;
            for (unsigned p = 0; p < op.fProxyCnt; ++p) {
                numTotalQuads += op.fProxies[p].fQuadCnt;
                auto* proxy = op.fProxies[p].fProxy;
                if (!proxy->instantiate(target->resourceProvider())) {
                    return;
                }
                SkASSERT(proxy->config() == config);
                SkASSERT(proxy->textureType() == textureType);
            }
            if (op.aaType() == GrAAType::kCoverage) {
                SkASSERT(aaType == GrAAType::kCoverage || aaType == GrAAType::kNone);
                aaType = GrAAType::kCoverage;
            }
        }

        bool coverageAA = GrAAType::kCoverage == aaType;
        sk_sp<GrGeometryProcessor> gp = TextureGeometryProcessor::Make(
                textureType, config, this->filter(), std::move(fTextureColorSpaceXform),
                std::move(fPaintColorSpaceXform), coverageAA, hasPerspective, domain,
                *target->caps().shaderCaps());
        GrPipeline::InitArgs args;
        args.fProxy = target->proxy();
        args.fCaps = &target->caps();
        args.fResourceProvider = target->resourceProvider();
        args.fFlags = 0;
        if (aaType == GrAAType::kMSAA) {
            args.fFlags |= GrPipeline::kHWAntialias_Flag;
        }

        auto clip = target->detachAppliedClip();
        // We'll use a dynamic state array for the GP textures when there are multiple ops.
        // Otherwise, we use fixed dynamic state to specify the single op's proxy.
        GrPipeline::DynamicStateArrays* dynamicStateArrays = nullptr;
        GrPipeline::FixedDynamicState* fixedDynamicState;
        if (numProxies > 1) {
            dynamicStateArrays = target->allocDynamicStateArrays(numProxies, 1, false);
            fixedDynamicState = target->allocFixedDynamicState(clip.scissorState().rect(), 0);
        } else {
            fixedDynamicState = target->allocFixedDynamicState(clip.scissorState().rect(), 1);
            fixedDynamicState->fPrimitiveProcessorTextures[0] = fProxies[0].fProxy;
        }
        const auto* pipeline =
                target->allocPipeline(args, GrProcessorSet::MakeEmptySet(), std::move(clip));
        using TessFn = decltype(&TextureOp::tess<SkPoint, Domain::kNo, GrAA::kNo>);
#define TESS_FN_AND_VERTEX_SIZE(Point, Domain, AA)                          \
    {                                                                       \
        &TextureOp::tess<Point, Domain, AA>,                                \
                sizeof(TextureGeometryProcessor::Vertex<Point, Domain, AA>) \
    }
        static constexpr struct {
            TessFn fTessFn;
            size_t fVertexSize;
        } kTessFnsAndVertexSizes[] = {
                TESS_FN_AND_VERTEX_SIZE(SkPoint,  Domain::kNo,  GrAA::kNo),
                TESS_FN_AND_VERTEX_SIZE(SkPoint,  Domain::kNo,  GrAA::kYes),
                TESS_FN_AND_VERTEX_SIZE(SkPoint,  Domain::kYes, GrAA::kNo),
                TESS_FN_AND_VERTEX_SIZE(SkPoint,  Domain::kYes, GrAA::kYes),
                TESS_FN_AND_VERTEX_SIZE(SkPoint3, Domain::kNo,  GrAA::kNo),
                TESS_FN_AND_VERTEX_SIZE(SkPoint3, Domain::kNo,  GrAA::kYes),
                TESS_FN_AND_VERTEX_SIZE(SkPoint3, Domain::kYes, GrAA::kNo),
                TESS_FN_AND_VERTEX_SIZE(SkPoint3, Domain::kYes, GrAA::kYes),
        };
#undef TESS_FN_AND_VERTEX_SIZE
        int tessFnIdx = 0;
        tessFnIdx |= coverageAA               ? 0x1 : 0x0;
        tessFnIdx |= (domain == Domain::kYes) ? 0x2 : 0x0;
        tessFnIdx |= hasPerspective           ? 0x4 : 0x0;

        size_t vertexSize = kTessFnsAndVertexSizes[tessFnIdx].fVertexSize;
        SkASSERT(vertexSize == gp->debugOnly_vertexStride());

        GrMesh* meshes = target->allocMeshes(numProxies);
        const GrBuffer* vbuffer;
        int vertexOffsetInBuffer = 0;
        int numQuadVerticesLeft = numTotalQuads * 4;
        int numAllocatedVertices = 0;
        void* vdata = nullptr;

        int m = 0;
        for (const auto& op : ChainRange<TextureOp>(this)) {
            int q = 0;
            for (unsigned p = 0; p < op.fProxyCnt; ++p) {
                int quadCnt = op.fProxies[p].fQuadCnt;
                auto* proxy = op.fProxies[p].fProxy;
                int meshVertexCnt = quadCnt * 4;
                if (numAllocatedVertices < meshVertexCnt) {
                    vdata = target->makeVertexSpaceAtLeast(
                            vertexSize, meshVertexCnt, numQuadVerticesLeft, &vbuffer,
                            &vertexOffsetInBuffer, &numAllocatedVertices);
                    SkASSERT(numAllocatedVertices <= numQuadVerticesLeft);
                    if (!vdata) {
                        SkDebugf("Could not allocate vertices\n");
                        return;
                    }
                }
                SkASSERT(numAllocatedVertices >= meshVertexCnt);

                (op.*(kTessFnsAndVertexSizes[tessFnIdx].fTessFn))(vdata, gp.get(), proxy, q,
                                                                  quadCnt);

                if (quadCnt > 1) {
                    meshes[m].setPrimitiveType(GrPrimitiveType::kTriangles);
                    sk_sp<const GrBuffer> ibuffer =
                            target->resourceProvider()->refQuadIndexBuffer();
                    if (!ibuffer) {
                        SkDebugf("Could not allocate quad indices\n");
                        return;
                    }
                    meshes[m].setIndexedPatterned(ibuffer.get(), 6, 4, quadCnt,
                                                  GrResourceProvider::QuadCountOfQuadBuffer());
                } else {
                    meshes[m].setPrimitiveType(GrPrimitiveType::kTriangleStrip);
                    meshes[m].setNonIndexedNonInstanced(4);
                }
                meshes[m].setVertexData(vbuffer, vertexOffsetInBuffer);
                if (dynamicStateArrays) {
                    dynamicStateArrays->fPrimitiveProcessorTextures[m] = proxy;
                }
                ++m;
                numAllocatedVertices -= meshVertexCnt;
                numQuadVerticesLeft -= meshVertexCnt;
                vertexOffsetInBuffer += meshVertexCnt;
                vdata = reinterpret_cast<char*>(vdata) + vertexSize * meshVertexCnt;
                q += quadCnt;
            }
        }
        SkASSERT(!numQuadVerticesLeft);
        SkASSERT(!numAllocatedVertices);
        target->draw(std::move(gp), pipeline, fixedDynamicState, dynamicStateArrays, meshes,
                     numProxies);
    }

    CombineResult onCombineIfPossible(GrOp* t, const GrCaps& caps) override {
        TRACE_EVENT0("skia", TRACE_FUNC);
        const auto* that = t->cast<TextureOp>();
        if (!GrColorSpaceXform::Equals(fTextureColorSpaceXform.get(),
                                       that->fTextureColorSpaceXform.get())) {
            return CombineResult::kCannotCombine;
        }
        if (!GrColorSpaceXform::Equals(fPaintColorSpaceXform.get(),
                                       that->fPaintColorSpaceXform.get())) {
            return CombineResult::kCannotCombine;
        }
        bool upgradeToCoverageAAOnMerge = false;
        if (this->aaType() != that->aaType()) {
            if (!((this->aaType() == GrAAType::kCoverage && that->aaType() == GrAAType::kNone) ||
                  (that->aaType() == GrAAType::kCoverage && this->aaType() == GrAAType::kNone))) {
                return CombineResult::kCannotCombine;
            }
            upgradeToCoverageAAOnMerge = true;
        }
        if (fFilter != that->fFilter) {
            return CombineResult::kCannotCombine;
        }
        auto thisProxy = fProxies[0].fProxy;
        auto thatProxy = that->fProxies[0].fProxy;
        if (fProxyCnt > 1 || that->fProxyCnt > 1 ||
            thisProxy->uniqueID() != thatProxy->uniqueID() || that->isChained()) {
            // We can't merge across different proxies (and we're disallowed from merging when
            // 'that' is chained. Check if we can be chained with 'that'.
            if (thisProxy->config() == thatProxy->config() &&
                thisProxy->textureType() == thatProxy->textureType() &&
                caps.dynamicStateArrayGeometryProcessorTextureSupport()) {
                return CombineResult::kMayChain;
            }
            return CombineResult::kCannotCombine;
        }
        fProxies[0].fQuadCnt += that->fQuads.count();
        fQuads.push_back_n(that->fQuads.count(), that->fQuads.begin());
        this->joinBounds(*that);
        fPerspective |= that->fPerspective;
        fDomain |= that->fDomain;
        if (upgradeToCoverageAAOnMerge) {
            fAAType = static_cast<unsigned>(GrAAType::kCoverage);
        }
        return CombineResult::kMerged;
    }

    GrAAType aaType() const { return static_cast<GrAAType>(fAAType); }
    GrSamplerState::Filter filter() const { return static_cast<GrSamplerState::Filter>(fFilter); }

    class Quad {
    public:
        Quad(const SkRect& srcRect, const GrPerspQuad& quad, GrQuadAAFlags aaFlags,
             SkCanvas::SrcRectConstraint constraint, GrColor color)
                : fSrcRect(srcRect)
                , fQuad(quad)
                , fColor(color)
                , fHasDomain(constraint == SkCanvas::kStrict_SrcRectConstraint)
                , fAAFlags(static_cast<unsigned>(aaFlags)) {
            SkASSERT(fAAFlags == static_cast<unsigned>(aaFlags));
        }
        const GrPerspQuad& quad() const { return fQuad; }
        const SkRect& srcRect() const { return fSrcRect; }
        GrColor color() const { return fColor; }
        Domain domain() const { return Domain(fHasDomain); }
        GrQuadAAFlags aaFlags() const { return static_cast<GrQuadAAFlags>(fAAFlags); }

    private:
        SkRect fSrcRect;
        GrPerspQuad fQuad;
        GrColor fColor;
        unsigned fHasDomain : 1;
        unsigned fAAFlags : 4;
    };
    struct Proxy {
        GrTextureProxy* fProxy;
        int fQuadCnt;
    };
    SkSTArray<1, Quad, true> fQuads;
    sk_sp<GrColorSpaceXform> fTextureColorSpaceXform;
    sk_sp<GrColorSpaceXform> fPaintColorSpaceXform;
    unsigned fFilter : 2;
    unsigned fAAType : 2;
    unsigned fPerspective : 1;
    unsigned fDomain : 1;
    // Used to track whether fProxy is ref'ed or has a pending IO after finalize() is called.
    unsigned fFinalized : 1;
    unsigned fCanSkipAllocatorGather : 1;
    unsigned fProxyCnt : 32 - 8;
    Proxy fProxies[1];

    typedef GrMeshDrawOp INHERITED;
};

}  // anonymous namespace

namespace GrTextureOp {

std::unique_ptr<GrDrawOp> Make(GrContext* context,
                               sk_sp<GrTextureProxy> proxy,
                               GrSamplerState::Filter filter,
                               GrColor color,
                               const SkRect& srcRect,
                               const SkRect& dstRect,
                               GrAAType aaType,
                               GrQuadAAFlags aaFlags,
                               SkCanvas::SrcRectConstraint constraint,
                               const SkMatrix& viewMatrix,
                               sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                               sk_sp<GrColorSpaceXform> paintColorSpaceXform) {
    return TextureOp::Make(context, std::move(proxy), filter, color, srcRect, dstRect, aaType,
                           aaFlags, constraint, viewMatrix, std::move(textureColorSpaceXform),
                           std::move(paintColorSpaceXform));
}

std::unique_ptr<GrDrawOp> Make(GrContext* context,
                               const GrRenderTargetContext::TextureSetEntry set[],
                               int cnt,
                               GrSamplerState::Filter filter,
                               GrColor color,
                               GrAAType aaType,
                               const SkMatrix& viewMatrix,
                               sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                               sk_sp<GrColorSpaceXform> paintColorSpaceXform) {
    return TextureOp::Make(context, set, cnt, filter, color, aaType, viewMatrix,
                           std::move(textureColorSpaceXform), std::move(paintColorSpaceXform));
}

}  // namespace GrTextureOp

#if GR_TEST_UTILS
#include "GrContext.h"
#include "GrContextPriv.h"
#include "GrProxyProvider.h"

GR_DRAW_OP_TEST_DEFINE(TextureOp) {
    GrSurfaceDesc desc;
    desc.fConfig = kRGBA_8888_GrPixelConfig;
    desc.fHeight = random->nextULessThan(90) + 10;
    desc.fWidth = random->nextULessThan(90) + 10;
    auto origin = random->nextBool() ? kTopLeft_GrSurfaceOrigin : kBottomLeft_GrSurfaceOrigin;
    GrMipMapped mipMapped = random->nextBool() ? GrMipMapped::kYes : GrMipMapped::kNo;
    SkBackingFit fit = SkBackingFit::kExact;
    if (mipMapped == GrMipMapped::kNo) {
        fit = random->nextBool() ? SkBackingFit::kApprox : SkBackingFit::kExact;
    }

    GrProxyProvider* proxyProvider = context->contextPriv().proxyProvider();
    sk_sp<GrTextureProxy> proxy = proxyProvider->createProxy(desc, origin, mipMapped, fit,
                                                             SkBudgeted::kNo,
                                                             GrInternalSurfaceFlags::kNone);

    SkRect rect = GrTest::TestRect(random);
    SkRect srcRect;
    srcRect.fLeft = random->nextRangeScalar(0.f, proxy->width() / 2.f);
    srcRect.fRight = random->nextRangeScalar(0.f, proxy->width()) + proxy->width() / 2.f;
    srcRect.fTop = random->nextRangeScalar(0.f, proxy->height() / 2.f);
    srcRect.fBottom = random->nextRangeScalar(0.f, proxy->height()) + proxy->height() / 2.f;
    SkMatrix viewMatrix = GrTest::TestMatrixPreservesRightAngles(random);
    GrColor color = SkColorToPremulGrColor(random->nextU());
    GrSamplerState::Filter filter = (GrSamplerState::Filter)random->nextULessThan(
            static_cast<uint32_t>(GrSamplerState::Filter::kMipMap) + 1);
    while (mipMapped == GrMipMapped::kNo && filter == GrSamplerState::Filter::kMipMap) {
        filter = (GrSamplerState::Filter)random->nextULessThan(
                static_cast<uint32_t>(GrSamplerState::Filter::kMipMap) + 1);
    }
    auto texXform = GrTest::TestColorXform(random);
    auto paintXform = GrTest::TestColorXform(random);
    GrAAType aaType = GrAAType::kNone;
    if (random->nextBool()) {
        aaType = (fsaaType == GrFSAAType::kUnifiedMSAA) ? GrAAType::kMSAA : GrAAType::kCoverage;
    }
    GrQuadAAFlags aaFlags = GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kLeft : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kTop : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kRight : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kBottom : GrQuadAAFlags::kNone;
    auto constraint = random->nextBool() ? SkCanvas::kStrict_SrcRectConstraint
                                         : SkCanvas::kFast_SrcRectConstraint;
    return GrTextureOp::Make(context, std::move(proxy), filter, color, srcRect, rect, aaType,
                             aaFlags, constraint, viewMatrix, std::move(texXform),
                             std::move(paintXform));
}

#endif
