#ifndef mapproxy_generator_surface_hpp_included_
#define mapproxy_generator_surface_hpp_included_

#include "vts-libs/vts/tileset/tilesetindex.hpp"
#include "vts-libs/vts/tileset/properties.hpp"

#include "../generator.hpp"

namespace vts = vadstena::vts;

namespace generator {

class SurfaceBase : public Generator {
public:
    SurfaceBase(const Config &config, const Resource &resource);

protected:
    boost::filesystem::path filePath(vts::File fileType) const;

private:
    virtual Task generateFile_impl(const FileInfo &fileInfo
                                   , const Sink::pointer &sink) const;

    virtual void generateMetatile(const vts::TileId &tileId
                                  , const Sink::pointer &sink
                                  , const SurfaceFileInfo &fileInfo
                                  , GdalWarper &warper) const = 0;

    virtual void generateMesh(const vts::TileId &tileId
                              , const Sink::pointer &sink
                              , const SurfaceFileInfo &fileInfo
                              , GdalWarper &warper) const = 0;

    virtual void generateNavtile(const vts::TileId &tileId
                                 , const Sink::pointer &sink
                                 , const SurfaceFileInfo &fileInfo
                                 , GdalWarper &warper) const = 0;
};

} // namespace generator

#endif // mapproxy_generator_surface_hpp_included_
