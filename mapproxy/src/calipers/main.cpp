#include <cstdlib>
#include <utility>
#include <functional>
#include <map>

#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/thread.hpp>
#include <boost/format.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include <opencv2/highgui/highgui.hpp>

#include "cpl_minixml.h"

#include "utility/streams.hpp"
#include "utility/buildsys.hpp"
#include "utility/openmp.hpp"
#include "utility/raise.hpp"
#include "utility/duration.hpp"
#include "utility/time.hpp"
#include "utility/enum-io.hpp"

#include "service/cmdline.hpp"

#include "geo/geodataset.hpp"
#include "geo/gdal.hpp"

#include "gdal-drivers/register.hpp"
#include "gdal-drivers/solid.hpp"

#include "vts-libs/registry.hpp"
#include "vts-libs/registry/po.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/csconvertor.hpp"
#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/basetypes.hpp"
#include "vts-libs/vts/math.hpp"
#include "vts-libs/vts/io.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace ba = boost::algorithm;
namespace ublas = boost::numeric::ublas;
namespace vr = vadstena::registry;
namespace vts = vadstena::vts;

UTILITY_GENERATE_ENUM(DatasetType,
    ((dem))
    ((ophoto))
)

class Calipers : public service::Cmdline {
public:
    Calipers()
        : service::Cmdline("calipers", BUILD_TARGET_VERSION)
        , demToOphotoScale_(3.0), tileFractionLimit_(32.0)
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd);

    void configure(const po::variables_map &vars);

    bool help(std::ostream &out, const std::string &what) const;

    int run();

    fs::path dataset_;
    std::string referenceFrameId_;
    boost::optional<DatasetType> datasetType_;
    double demToOphotoScale_;
    double tileFractionLimit_;

    const vr::ReferenceFrame *referenceFrame_;
};

void Calipers::configuration(po::options_description &cmdline
                            , po::options_description &config
                            , po::positional_options_description &pd)
{
    vr::registryConfiguration(cmdline, vr::defaultPath());

    cmdline.add_options()
        ("dataset", po::value(&dataset_)->required()
         , "Path to GDAL dataset to examine.")
        ("referenceFrame", po::value(&referenceFrameId_)->required()
         , "Reference frame.")
        ("datasetType", po::value<DatasetType>()
         , "Dataset type (dem or ophoto). Mandatory only "
         "if autodetect fails.")

        ("demToOphotoScale", po::value(&demToOphotoScale_)
         ->default_value(demToOphotoScale_)->required()
         , "Inverse scale between DEM's resolution and resolution of "
         "most detailed orthophoto that can be draped on it. "
         "Used for bottom LOD calculation. "
         "To get 2x better ophoto (i.e. resolution scale 1/2) use 2.")

        ("tileFractionLimit", po::value(&tileFractionLimit_)
         ->default_value(tileFractionLimit_)->required()
         , "Fraction of tile when rastrization algorithm stops."
         "Inverse value, 4 means 1/4 of tile.")
        ;

    pd.add("dataset", 1)
        .add("referenceFrame", 1)
        ;

    (void) config;
}

void Calipers::configure(const po::variables_map &vars)
{
    vr::registryConfigure(vars);

    dataset_ = fs::absolute(dataset_);

    referenceFrame_ = &vr::system.referenceFrames(referenceFrameId_);

    if (vars.count("datasetType")) {
        datasetType_ = vars["datasetType"].as<DatasetType>();
    }

    LOG(info3, log_)
        << "Config:"
        << "\n\tdataset = " << dataset_
        << "\n\treferenceFrame = " << referenceFrameId_
        << "\n"
        ;

    (void) vars;
}

bool Calipers::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        // program help
        out << ("calipers dataset referenceFrame [options]\n"
                "    Measures GDAL dataset in given reference frame.\n"
                "\n"
                );

        return true;
    }

    return false;
}

DatasetType detectType(const geo::GeoDataset::Descriptor &ds
                       , const boost::optional<DatasetType> &forcedType)
{
    if (forcedType) { return *forcedType; }

    if (ds.bands >= 3) { return DatasetType::ophoto; }

    if (ds.bands != 1) {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot autodetect dataset type, unsupported number of bands ("
            << ds.bands << ").";
    }

    if (ds.dataType == GDT_Byte) {
        // probably monochromatic orthophoto
        return DatasetType::ophoto;
    }

    // anything else is DEM
    return DatasetType::dem;
}

class PointGrid {
public:
    PointGrid(const math::Size2 &size)
        : size_(size), grid_(math::area(size))
    {}

    math::Point2d& operator()(std::size_t j, std::size_t i) {
        return grid_[j * size_.width + i];
    }

    const math::Point2d& operator()(std::size_t j, std::size_t i) const {
        return grid_[j * size_.width + i];
    }

private:
    math::Size2 size_;
    math::Points2d grid_;
};

typedef std::array<boost::optional<math::Point2d>, 4> OptCorners;

inline bool partial(const OptCorners &c) {
    auto b([&](int i) -> bool { return c[i]; });
    auto sum(b(0) + b(1) + b(2) + b(3));
    return (sum > 0) && (sum < 4);
}

inline bool valid(const OptCorners &c) {
    return c[0] && c[1] && c[2] && c[3];
}

class Node {
public:
    Node(const geo::GeoDataset::Descriptor &ds
         , const vts::NodeInfo &node, const math::Size2 &steps)
        : ds(ds), node(node), ds2node(ds.srs, node.srs())
        , extents(ds.extents)
        , grid(steps.width + 1, steps.height + 1, (unsigned char)(0))
        , localExtents(math::InvalidExtents{})
        , projectedGrid(math::Size2(steps.width + 1, steps.height + 1))
        , stepInPixels(double(ds.size.width) / steps.width
                       , double(ds.size.height) / steps.height)
        , localLod(), lod(), minLod_(), tileRange_(math::InvalidExtents{})
    {
        step = math::size(extents);
        step.width /= steps.width;
        step.height /= steps.height;
    }

    bool run(double invGsdScale, double tileFractionLimit) {
        if (sample(invGsdScale, tileFractionLimit)) {
            refine();
            minLod();
            return true;
        }
        return false;
    }

    vts::Ranges ranges() const {
        return vts::Ranges(vts::LodRange(minLod_, lod), tileRange_
                           , vts::Ranges::FromBottom{});
    }

    const std::string& srs() const { return node.srs(); }

private:
    bool convert(math::Point2 &c, double x, double y) {
        try {
            // try to convert corner
            c = ds2node(math::Point2d(x, y));
            // check if it is inside the node
            if (!node.inside(c)) { return true; }

            // update local extents
            update(localExtents, c);
        } catch (...) {
            return true;
        }
        return false;
    }

    boost::optional<math::Point2> convert(double x, double y) {
        math::Point2 c;
        if (convert(c, x, y)) { return boost::none; }
        return c;
    }

    bool sample(double invGsdScale, double tileFractionLimit);
    void refine();
    void minLod();

    void divideBorderBlock(math::Size2f blockPxSize
                           , const math::Extents2 &extents
                           , const OptCorners &corners);

    geo::GeoDataset::Descriptor ds;
    vts::NodeInfo node;
    vts::CsConvertor ds2node;
    math::Extents2 extents;
    math::Size2f step;
    cv::Mat_<unsigned char> grid;

    math::Extents2 localExtents;
    PointGrid projectedGrid;
    math::Size2f stepInPixels;

    math::Size2f sourceBlockLimit;
    vts::Lod localLod;
    vts::Lod lod;
    vts::Lod minLod_;
    vts::TileRange tileRange_;
};

bool Node::sample(double invGsdScale, double tileFractionLimit)
{
    const auto &nodeId(node.nodeId());
    const auto paneSize(math::size(node.extents()));

    // size of dataset
    const auto es(size(extents));
    // center of dataset
    const auto dsCenter(math::center(extents));

    // calculate pixel and halfpixel size
    const math::Size2f px((es.width / ds.size.height)
                          , (es.width / ds.size.height));
    const math::Size2f hpx(px.width / 2.0, px.height / 2.0);

    // best (local) LOD computed for this node
    // make_optional used to get rid of "maybe uninitialized" GCC warning
    auto bestLod(boost::make_optional<double>(false, 0.0));

    double bestDistance(std::numeric_limits<double>::max());

    // process whole grid
    double y(extents.ll(1));
    for (int j(0); j < grid.rows; ++j, y += step.height) {
        double x(extents.ll(0));
        for (int i(0); i < grid.cols; ++i, x += step.width) {

            // try to convert grid point to node's SRS
            if (convert(projectedGrid(j, i), x, y)) {
                continue;
            }

            // valid grid point, mark
            grid(j, i) = 255;

            // make point a pixel center, fix coordinates on boundary
            math::Point2d p(x, y);
            if (i == 0) { p(0) += hpx.width; }
            else if (i == grid.cols) { p(0) -= hpx.width; }
            if (j == 0) { p(1) += hpx.height; }
            else if (j == grid.rows) { p(1) -= hpx.height; }


            // convert pixel aroung grid point to node's SRS
            std::array<math::Point2d, 4> corners;
            if (convert(corners[0], p(0) - hpx.width
                             , p(1) - hpx.height)
                || convert(corners[1], p(0) - hpx.width
                                , p(1) + hpx.height)
                || convert(corners[2], p(0) + hpx.width
                                , p(1) + hpx.height)
                || convert(corners[3], p(0) + hpx.width
                                , p(1) - hpx.height))
            {
                continue;
            }

            // we have valid quadrilateral

            // calculate distance between pixel center and dataset center
            const auto distance(ublas::norm_2(p - dsCenter));

            // futher than previous best point?
            if (distance >= bestDistance) { continue; }

            // calculate (approximate) projected quad area
            const auto pxArea
                (vts::triangleArea(corners[0], corners[1], corners[2])
                 + vts::triangleArea(corners[2], corners[3], corners[0]));

            // calculate best lod:
            // divide node's pane area by tiles area
            // apply square root to get number of tiles per side
            // and log2 to get lod
            // NB: log2(sqrt(a)) = 0.5 * log2(a)
            // NB: inverse GSD scale is applied to node pane area
            // NB: calculated in two passes to overcome problem with huge
            // numbers (area(paneSize) for webmercator is realy huge and loses
            // precision)
            const auto tmp((paneSize.width * invGsdScale * invGsdScale)
                           / (pxArea * vr::BoundLayer::tileArea()));
            const auto lod(0.5 * std::log2(tmp * paneSize.height));

            // sanity check: no negative LOD
            if (lod >= 0.0) {
                bestLod = lod;
                bestDistance = distance;
            }
        }
    }

    if (!bestLod) { return false; }

    // round to integral LOD
    vts::Lod computed(std::ceil(*bestLod));

    // lowest root's child at computed (local) lod
    const vts::NodeInfo lowestChild(node.referenceFrame()
                                    , vts::lowestChild(nodeId, computed));

    // check whether curren subtree root can produce tiles at computed
    // lod
    if (!compatible(lowestChild, node)) { return false; }

    localLod = computed;
    lod = nodeId.lod + computed;

    // set source block size limit 1/4 of source tile size
    sourceBlockLimit.width
        = (vr::BoundLayer::tileSize().width
           / (invGsdScale * tileFractionLimit));
    sourceBlockLimit.height
        = (vr::BoundLayer::tileSize().height
           / (invGsdScale * tileFractionLimit));

    // done
    return true;
}

void Node::divideBorderBlock(math::Size2f blockPxSize
                             , const math::Extents2 &extents
                             , const OptCorners &corners)
{
    if ((blockPxSize.width < sourceBlockLimit.width)
        && (blockPxSize.height < sourceBlockLimit.height))
    {
        // too little source
        return;
    }

    // halve pixel size
    blockPxSize.width /= 2.0;
    blockPxSize.height /= 2.0;

    const auto ec(math::center(extents));

    // try to transform 5 points on the cross in the center of block
    auto center(convert(ec(0), ec(1)));
    auto left(convert(extents.ll(0), ec(1)));
    auto right(convert(extents.ur(0), ec(1)));
    auto lower(convert(ec(0), extents.ll(1)));
    auto upper(convert(ec(0), extents.ur(1)));

    // construct 4 sub-blocks and try again
    {
        // ll
        OptCorners c{{corners[0], left, center, lower}};
        if (partial(c)) {
            divideBorderBlock(blockPxSize
                              , math::Extents2(extents.ll, ec), c);
        }
    }

    {
        // ul
        OptCorners c{{left, corners[1], upper, center}};
        if (partial(c)) {
            divideBorderBlock(blockPxSize
                              , math::Extents2(extents.ll(0), ec(1)
                                               , ec(0), extents.ur(1))
                              , c);
        }
    }

    {
        // ur
        OptCorners c{{center, upper, corners[2], right}};
        if (partial(c)) {
            divideBorderBlock(blockPxSize
                              , math::Extents2(ec, extents.ur), c);
        }
    }

    {
        // lr
        OptCorners c{{lower, center, right, corners[3]}};
        if (partial(c)) {
            divideBorderBlock(blockPxSize
                              , math::Extents2(ec(0), extents.ll(1)
                                               , extents.ur(0), ec(1))
                              , c);
        }
    }
}

void Node::refine()
{
    double y(extents.ll(1));
    for (int j = 1; j < grid.rows; ++j, y += step.height) {
        double x(extents.ll(0));

        bool ppx(grid(j - 1, 0));
        bool pcx(grid(j, 0));
        for (int i = 1; i < grid.cols; ++i, x += step.width) {
            bool px(grid(j - 1, i));
            bool cx(grid(j, i));

            int corners(px + cx + ppx + pcx);
            if (corners && (corners < 4)) {
                // border block
                math::Extents2 be(x, y, x + step.width, y + step.height);

                // construct corners to have same order as
                // math::vertices(extents)
                OptCorners corners;
                if (ppx) { corners[0] = projectedGrid(j - 1, i - 1); }
                if (pcx) { corners[1] = projectedGrid(j, i - 1); }
                if (cx) { corners[2] = projectedGrid(j, i); }
                if (px) { corners[3] = projectedGrid(j - 1, i); }

                divideBorderBlock(stepInPixels, be, corners);
            }

            ppx = px;
            pcx = cx;
        }
    }

    const auto ts(vts::tileSize(node.extents(), localLod));
    const auto origin(math::ul(node.extents()));

    auto point2tile([&](const math::Point2d &p)
    {
        return vts::TileRange::point_type
            ((p(0) - origin(0)) / ts.width
             , (origin(1) - p(1)) / ts.height);
    });

    // compose tile range from all 4 corners of localExtents
    update(tileRange_, point2tile(ll(localExtents)));
    update(tileRange_, point2tile(ul(localExtents)));
    update(tileRange_, point2tile(ur(localExtents)));
    update(tileRange_, point2tile(lr(localExtents)));
}

void Node::minLod()
{
    // size of root pane
    const auto paneSize(math::size(node.extents()));

    // size of local extents
    const auto localSize(math::size(localExtents));

    // calculate lod from area division
    auto lod(0.5 * std::log2((paneSize.width / localSize.width)
                             * (paneSize.height / localSize.height)));
    if (lod < 0.0) {
        lod = 0.0;
    } else {
        // floor
        lod = std::floor(lod);
    }

    // make global
    minLod_ = node.nodeId().lod + lod;
}

double computeGsd(const geo::GeoDataset::Descriptor &ds
                  , const vr::ReferenceFrame &rf)
{
    auto navSrs(vr::system.srs(rf.model.navigationSrs).srsDef.reference());

    ::OGRSpatialReference latlon;
    if (OGRERR_NONE != latlon.CopyGeogCSFrom(&navSrs)) {
        LOGTHROW(err3, std::runtime_error)
            << "Cannot copy GeoCS from navigation SRS.";
    }

    const auto dsCenter(math::center(ds.extents));

    const auto llCenter(geo::CsConvertor(ds.srs, latlon)(dsCenter));

    // construct tmerc
    ::OGRSpatialReference tm;
    if (OGRERR_NONE != tm.CopyGeogCSFrom(&navSrs)) {
        LOGTHROW(err3, std::runtime_error)
            << "Cannot copy GeoCS from navigation SRS.";
    }
    if (OGRERR_NONE != tm.SetTM(llCenter(1), llCenter(0), 1.0, 0.0, 0.0)) {
        LOGTHROW(err3, std::runtime_error)
            << "Cannot set tmerc.";
    }

    // compute dataset's half-pixel size
    auto hpx(math::size(ds.extents));
    hpx.width /= (2.0 * ds.size.width);
    hpx.height /= (2.0 * ds.size.height);

    // project pixel from dataset center to tmerc
    const geo::CsConvertor ds2tm(ds.srs, tm);
    std::array<math::Point2d, 4> corners;
    corners[0] = ds2tm(math::Point2d(dsCenter(0) - hpx.width
                                     , dsCenter(1) - hpx.height));
    corners[1] = ds2tm(math::Point2d(dsCenter(0) - hpx.width
                                     , dsCenter(1) + hpx.height));
    corners[2] = ds2tm(math::Point2d(dsCenter(0) + hpx.width
                                     , dsCenter(1) + hpx.height));
    corners[3] = ds2tm(math::Point2d(dsCenter(0) + hpx.width
                                     , dsCenter(1) - hpx.height));

    // compute (aproximage) area of pixel in tmerc
    const auto pxArea
        (vts::triangleArea(corners[0], corners[1], corners[2])
         + vts::triangleArea(corners[2], corners[3], corners[0]));

    // compute gsd from pixel area
    return std::sqrt(pxArea);
}

int Calipers::run()
{
    const auto ds(geo::GeoDataset::open(dataset_).descriptor());

    const auto datasetType(detectType(ds, datasetType_));

    const auto gsd(computeGsd(ds, *referenceFrame_));
    std::cout << "gsd: " << gsd << "\n";

    // inverse GSD scale
    const double invGsdScale((datasetType == DatasetType::dem)
                             ? demToOphotoScale_
                             : 1.0);

    // division of source dataset
    math::Size2 steps(255, 255);

    const auto nodes(vts::NodeInfo::nodes(*referenceFrame_));

    UTILITY_OMP(parallel for)
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        Node node(ds, nodes[nodeIndex], steps);

        if (node.run(invGsdScale, tileFractionLimit_)) {
            auto r(node.ranges());
            std::cout << node.srs() << ": " << r.lodRange();
            char sep('/');
            for (auto lod : r.lodRange()) {
                std::cout << sep << r.tileRange(lod);
                sep = ';';
            }
            std::cout << '\n';
        }
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    gdal_drivers::registerAll();
    return Calipers()(argc, argv);
}
