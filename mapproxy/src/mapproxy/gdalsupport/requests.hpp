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

#ifndef mapproxy_gdalsupport_requests_hpp_included_
#define mapproxy_gdalsupport_requests_hpp_included_

#include "../gdalsupport.hpp"
#include "./types.hpp"


class ShRequestBase {
public:
    virtual ~ShRequestBase() {}

    void done() { done_impl(); }

private:
    virtual void done_impl() = 0;
};

class ShRaster : boost::noncopyable {
public:
    ShRaster(const GdalWarper::RasterRequest &other
             , ManagedBuffer &sm, ShRequestBase *owner);

    ~ShRaster();

    operator GdalWarper::RasterRequest() const;

    /** Steals response.
     */
    cv::Mat* response();
    void response(bi::interprocess_mutex &mutex, cv::Mat *response);

private:
    ManagedBuffer &sm_;
    ShRequestBase *owner_;

    GdalWarper::RasterRequest::Operation operation_;
    String dataset_;
    String srs_;
    geo::SrsDefinition::Type srsType_;
    math::Extents2 extents_;
    math::Size2 size_;
    geo::GeoDataset::Resampling resampling_;
    String mask_;

    // response matrix
    cv::Mat *response_;
};

struct ShHeightCodeConfig {
    String workingSrs_;
    geo::SrsDefinition::Type workingSrsType_;

    String outputSrs_;
    geo::SrsDefinition::Type outputSrsType_;

    boost::optional<StringVector> layers_;

    boost::optional<math::Extents2> clipWorkingExtents_;

    geo::VectorFormat format_;

    heightcoding::Mode mode_;

    ShHeightCodeConfig(const geo::heightcoding::Config &config
                       , ManagedBuffer &sm);

    operator geo::heightcoding::Config() const;
};

struct ShDemDataset {
    String dataset;
    String geoidGrid;

    ShDemDataset(const DemDataset &demDataset, ManagedBuffer &sm);

    DemDataset demDataset() const;
};

typedef bi::vector<ShDemDataset, bi::allocator<ShDemDataset, SegmentManager>>
    ShDemDatasetList;

class ShHeightCode : boost::noncopyable {
public:
    ShHeightCode(const std::string &vectorDs
                 , const DemDataset::list &rasterDs
                 , const geo::heightcoding::Config &config
                 , const boost::optional<std::string> &vectorGeoidGrid
                 , ManagedBuffer &sm, ShRequestBase *owner);

    ~ShHeightCode();

    std::string vectorDs() const;

    DemDataset::list rasterDs() const;

    geo::heightcoding::Config config() const;

    boost::optional<std::string> vectorGeoidGrid() const;

    /** Steals response.
     */
    GdalWarper::Heightcoded* response();

    void response(bi::interprocess_mutex &mutex
                  , GdalWarper::Heightcoded *response);

private:
    ManagedBuffer &sm_;
    ShRequestBase *owner_;
    String vectorDs_;
    ShDemDatasetList rasterDs_;
    ShHeightCodeConfig config_;
    String vectorGeoidGrid_;

    // response memory block
    GdalWarper::Heightcoded *response_;
};

struct ShNavtile {
    String path;
    String raw;
    math::Extents2 extents;
    String sdsSrs;
    String navSrs;
    vts::NavTile::HeightRange heightRange;

    ShNavtile(const GdalWarper::Navtile &navtile, ManagedBuffer &sm);

    GdalWarper::Navtile navtile(bool noRaw = false) const;

    ConstBlock rawData() const;
};

class ShNavHeightCode : boost::noncopyable {
public:
    ShNavHeightCode(const std::string &vectorDs
                    , const GdalWarper::Navtile &navtile
                    , const geo::heightcoding::Config &config
                    , const std::string &fallbackDs
                    , const boost::optional<std::string> &geoidGrid
                    , ManagedBuffer &sm, ShRequestBase *owner);

    ~ShNavHeightCode();

    std::string vectorDs() const;

    GdalWarper::Navtile navtile(bool noRaw = false) const;

    geo::heightcoding::Config config() const;

    ConstBlock rawData() const;

    std::string fallbackDs() const;

    boost::optional<std::string> geoidGrid() const;

    /** Steals response.
     */
    GdalWarper::Heightcoded* response();

    void response(bi::interprocess_mutex &mutex
                  , GdalWarper::Heightcoded *response);

private:
    ManagedBuffer &sm_;
    ShRequestBase *owner_;

    String vectorDs_;
    ShNavtile navtile_;
    ShHeightCodeConfig config_;
    String fallbackDs_;
    String geoidGrid_;

    // response memory block
    GdalWarper::Heightcoded *response_;
};

#endif // mapproxy_gdalsupport_requests_hpp_included_
