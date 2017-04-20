/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "utility/raise.hpp"

#include "../support/metatile.hpp"
#include "../support/geo.hpp"
#include "../support/grid.hpp"
#include "../support/srs.hpp"
#include "../support/mesh.hpp"

#include "./metatile.hpp"

namespace {

/** NB: Do Not Change!
 *
 * This constant has huge impact on dataset stability. Changing this value may
 * break data already served to the outer world.
 */
const int metatileSamplesPerTileBinLog(3);

// real size computed from binary logarithm above
const int metatileSamplesPerTile(1 << metatileSamplesPerTileBinLog);

typedef vs::Range<double> HeightRange;

/** One sample in metatile.
 */
struct Sample {
    bool valid;
    math::Point3 value;
    math::Point3 min;
    math::Point3 max;
    HeightRange heightRange;
    vts::GeomExtents ge;

    Sample() : valid(false), heightRange(HeightRange::emptyRange()) {}

    Sample(double x, double y, const cv::Vec3d &value
           , const vts::CsConvertor &conv
           , const vts::CsConvertor &navConv
           , const vts::CsConvertor &geConv)
        : valid(true)
        , value(conv(math::Point3(x, y, value[0])))
        , min(conv(math::Point3(x, y, value[1])))
        , max(conv(math::Point3(x, y, value[2])))
        , heightRange(navConv(math::Point3(x, y, value[1]))(2)
                      , navConv(math::Point3(x, y, value[2]))(2))
        , ge(geConv(math::Point3(x, y, value[1]))(2)
             , geConv(math::Point3(x, y, value[2]))(2)
             , geConv(math::Point3(x, y, value[0]))(2))
    {}
};

const Sample* getSample(const Sample &sample)
{
    return (sample.valid ? &sample : nullptr);
}

const math::Point3* getValue(const Sample *sample)
{
    return ((sample && sample->valid) ? &sample->value : nullptr);
}

const math::Point3* getValue(const Sample &sample)
{
    return (sample.valid ? &sample.value : nullptr);
}

inline bool validSample(double value)
{
    return (value >= -1e6);
}

class ValueMinMaxSampler {
public:
    ValueMinMaxSampler(const GdalWarper::Raster &dem)
        : dem_(dem)
    {}

    boost::optional<cv::Vec3d> operator()(int i, int j) const {
        // first, try exact value
        const auto &v(dem_->at<cv::Vec3d>(j, i));
        if (validSample(v[0])) { return v; }

        // output vector and count of valid samples
        boost::optional<cv::Vec3d> outFull;
        outFull = boost::in_place(0, std::numeric_limits<double>::max()
                                  , std::numeric_limits<double>::lowest());
        auto &out(*outFull);
        int count(0);

        for (int jj(-1); jj <= +1; ++jj) {
            for (int ii(-1); ii <= +1; ++ii) {
                // ignore current point
                if (!(ii || jj)) { continue; }

                auto x(i + ii), y(j + jj);
                // check bounds
                if ((x < 0) || (x >= dem_->cols)
                    || (y < 0) || (y >= dem_->rows))
                    { continue; }

                const auto &v(dem_->at<cv::Vec3d>(y, x));
                if (validSample(v[0])) {
                    out[0] += v[0];
                    out[1] = std::min(out[1], v[1]);
                    out[2] = std::max(out[2], v[2]);
                    ++count;
                }
            }
        }

        if (!count) { return boost::none; }

        out[0] /= count;
        return out;
    }

private:
    GdalWarper::Raster dem_;
};

typedef vts::MetaNode::Flag MetaFlag;
typedef vts::TileIndex::Flag TiFlag;

inline MetaFlag::value_type ti2metaFlags(TiFlag::value_type ti)
{
    MetaFlag::value_type meta(MetaFlag::allChildren);
    if (ti & TiFlag::mesh) {
        meta |= MetaFlag::geometryPresent;
    }
    if (ti & TiFlag::navtile) {
        meta |= MetaFlag::navtilePresent;
    }

    return meta;
}

} // namespace

vts::MetaTile
metatileFromDem(const vts::TileId &tileId, Sink &sink, Arsenal &arsenal
                , const Resource &resource
                , const vts::TileIndex &tileIndex
                , const std::string &demDataset
                , const boost::optional<std::string> &geoidGrid
                , const MaskTree &maskTree
                , const boost::optional<int> &displaySize)
{
    auto blocks(metatileBlocks(resource, tileId));

    if (blocks.empty()) {
        utility::raise<NotFound>
            ("Metatile completely outside of configured range.");
    }

    const auto &rf(*resource.referenceFrame);

    vts::MetaTile metatile(tileId, rf.metaBinaryOrder);

    auto setChildren([&](const vts::TileId &nodeId, vts::MetaNode &node)
                     -> void
    {
        // build children from tile index
        //
        // TODO: let tileindex generate validity flag for all children
        // in this block
        for (const auto &child : vts::children(nodeId)) {
            /** some subtrees may have false positives
             * (i.e. melown2015 polar caps)
             *
             * Combine tile index with node validity.
             *
             * TODO: optimize
             */
            bool valid(tileIndex.validSubtree(child));
            valid = valid && vts::NodeInfo(rf, child).valid();
            node.setChildFromId(child, valid);
        }
    });

    auto generateUnproductiveNodes([&](const MetatileBlock &block
                                       , const math::Size2 &bSize) -> void
    {
        const auto &view(block.view);
        for (int j(0), je(bSize.height); j < je; ++j) {
            for (int i(0), ie(bSize.width); i < ie; ++i) {
                // ID of current tile
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                // build metanode
                vts::MetaNode node;
                node.flags(ti2metaFlags(tileIndex.get(nodeId)));
                setChildren(nodeId, node);
                metatile.set(nodeId, node);
            }
        }
    });

    for (const auto &block : blocks) {
        const auto &view(block.view);
        auto extents = block.extents;
        const auto es(math::size(extents));
        const math::Size2 bSize(vts::tileRangesSize(view));

        if (!block.commonAncestor.productive()) {
            // unproductive node
            generateUnproductiveNodes(block, bSize);
            continue;
        }

        const math::Size2 gridSize
            (bSize.width * metatileSamplesPerTile + 1
             , bSize.height * metatileSamplesPerTile + 1);

        LOG(info1) << "Processing metatile block ["
                   << vts::tileId(tileId.lod, block.view.ll)
                   << ", " << vts::tileId(tileId.lod, block.view.ur)
                   << "], ancestor: " << block.commonAncestor.nodeId()
                   << ", tile offset: " << block.offset
                   << ", size in tiles: " << vts::tileRangesSize(block.view)
                   << ".";

        // warp value intentionally by average filter
        auto dem(arsenal.warper.warp
                 (GdalWarper::RasterRequest
                  (GdalWarper::RasterRequest::Operation::valueMinMax
                   , demDataset
                   , vr::system.srs(block.srs).srsDef
                   // add half pixel to warp in grid coordinates
                   , extentsPlusHalfPixel
                   (extents, { gridSize.width - 1, gridSize.height - 1 })
                   , gridSize
                   , geo::GeoDataset::Resampling::dem)
                  , sink));

        sink.checkAborted();

        Grid<Sample> grid(gridSize);

        // tile size in grid and in real SDS
        math::Size2f gts
            (es.width / (metatileSamplesPerTile * bSize.width)
             , es.height / (metatileSamplesPerTile * bSize.height));
        math::Size2f ts(es.width / bSize.width
                        , es.height / bSize.height);

        auto conv(sds2phys(block.commonAncestor, geoidGrid));
        auto navConv(sds2nav(block.commonAncestor, geoidGrid));
        auto geConv(sdsg2sdsr(block.commonAncestor, geoidGrid));

        // grid mask
        const ShiftMask rfmask(block, metatileSamplesPerTile, maskTree);

        // fill in grid
        ValueMinMaxSampler vmm(dem);
        for (int j(0), je(gridSize.height); j < je; ++j) {
            auto y(extents.ur(1) - j * gts.height);
            for (int i(0), ie(gridSize.width); i < ie; ++i) {
                // work only with pixels not masked by combined mask
                if (!rfmask(i, j)) { continue; }

                auto value(vmm(i, j));

                // skip out invalid data
                if (!value) { continue; }

                auto x(extents.ll(0) + i * gts.width);

                // compute all 3 world points (value, min, max) and height range
                // in navigation space
                grid(i, j) = { x, y, *value, conv, navConv, geConv };
            }
        }

        // release shared data
        dem.reset();

        // generate metatile content
        for (int j(0), je(bSize.height); j < je; ++j) {
            for (int i(0), ie(bSize.width); i < ie; ++i) {
                // ID of current tile
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                // build metanode
                vts::MetaNode node;
                node.flags(ti2metaFlags(tileIndex.get(nodeId)));
                bool geometry(node.geometry());
                bool navtile(node.navtile());

                // compute tile extents and height range
                auto heightRange(HeightRange::emptyRange());
                math::Extents3 te(math::InvalidExtents{});
                double area(0.0);
                int triangleCount(0);
                double avgHeightSum(0.f);
                int avgHeightCount(0);

                // process all node's vertices in grid
                for (int jj(0); jj <= metatileSamplesPerTile; ++jj) {
                    auto yy(j * metatileSamplesPerTile + jj);
                    for (int ii(0); ii <= metatileSamplesPerTile; ++ii) {
                        auto xx(i * metatileSamplesPerTile + ii);

                        const auto *p(getSample(grid(xx, yy)));

                        // update tile extents (if sample valid)
                        if (p) {
                            // update by both minimum and maximum
                            math::update(te, p->min);
                            math::update(te, p->max);
                            vts::update(node.geomExtents, p->ge);
                            avgHeightSum += p->ge.surrogate;
                            ++avgHeightCount;
                        }

                        if (geometry && ii && jj) {
                            // compute area of the quad composed of 1 or 2
                            // triangles
                            auto qa(quadArea
                                    (getValue(grid(xx - 1, yy - 1))
                                     , getValue(p)
                                     , getValue(grid(xx - 1, yy))
                                     , getValue(grid(xx, yy - 1))));
                            area += std::get<0>(qa);
                            triangleCount += std::get<1>(qa);
                        }

                        if (p && navtile) {
                            heightRange
                                = vs::unite(heightRange, p->heightRange);
                        }
                    }
                }

                // build children from tile index
                setChildren(nodeId, node);

                // set extents
                node.extents = vr::normalizedExtents(rf, te);

                // build height range
                node.heightRange.min = std::floor(heightRange.min);
                node.heightRange.max = std::ceil(heightRange.max);

                if (!triangleCount) {
                    // reset content flags
                    node.geometry(geometry = false);
                    node.navtile(navtile = false);
                    // reset height range
                    heightRange = heightRange.emptyRange();
                    // reset geom extents
                    node.geomExtents = {};
                }

                // calculate texel size and surrogate
                if (geometry) {
                    // set credits
                    node.updateCredits(resource.credits);

                    if (displaySize) {
                        // use display size
                        node.applyDisplaySize(true);
                        node.displaySize = *displaySize;
                    } else {
                        // use texel size
                        node.applyTexelSize(true);

                        // calculate texture size using node mask
                        auto textureArea([&]() -> double
                        {
                            math::Size2 size(metatileSamplesPerTile
                                             , metatileSamplesPerTile);

                            // return scaled coverage; NB: triangle covers half
                            // of pixel so real area is in pixels is half of
                            // number of pixels
                            return ((triangleCount
                                     * vr::BoundLayer::tileArea())
                                    / (2.0 * math::area(size)));
                        }());

                        // calculate texel size
                        node.texelSize = std::sqrt(area / textureArea);
                    }

                    // surrogate
                    if (avgHeightCount) {
                        node.geomExtents.surrogate
                            = (avgHeightSum / avgHeightCount);
                    }
                }

                // store metata node
                metatile.set(nodeId, node);
            }
        }
    }

    return metatile;
}
