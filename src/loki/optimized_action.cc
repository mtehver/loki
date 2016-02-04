#include <functional>
#include <unordered_map>
#include <boost/property_tree/info_parser.hpp>

#include <valhalla/baldr/json.h>
#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/midgard/logging.h>

#include "loki/service.h"
#include "loki/search.h"

using namespace prime_server;
using namespace valhalla::baldr;
using namespace valhalla::loki;

namespace {
  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const headers_t::value_type JSON_MIME{"Content-type", "application/json;charset=utf-8"};
  const headers_t::value_type JS_MIME{"Content-type", "application/javascript;charset=utf-8"};
  std::vector<float> max_manytomany_list;

  void check_locations(const size_t location_count, const size_t max_locations) {
    //check that location size does not exceed max.
    if (location_count > max_locations)
      throw std::runtime_error("Exceeded max locations of " + std::to_string(max_locations) + ".");
  }

  void check_distance(const GraphReader& reader, const std::vector<Location>& locations, const size_t origin, const size_t start, const size_t end, float matrix_max_distance) {
    auto max_location_distance = 0.f;

    //see if any locations pairs are unreachable or too far apart
    auto lowest_level = reader.GetTileHierarchy().levels().rbegin();
    //one to many should be distance between:a,b a,c ; many to one: a,c b,c ; many to many should be all pairs
    for(size_t i = start; i < end; ++i) {
      //check connectivity
      uint32_t a_id = lowest_level->second.tiles.TileId(locations[origin].latlng_);
      uint32_t b_id = lowest_level->second.tiles.TileId(locations[i].latlng_);
      if(!reader.AreConnected({a_id, lowest_level->first, 0}, {b_id, lowest_level->first, 0}))
        throw std::runtime_error("Locations are in unconnected regions. Go check/edit the map at osm.org");

      //check if distance between latlngs exceed max distance limit the chosen matrix type
      auto path_distance = locations[origin].latlng_.Distance(locations[i].latlng_);

      //only want to log the maximum distance between 2 locations for matrix
      if (path_distance >= max_location_distance)
        max_manytomany_list.push_back(path_distance);

      if (path_distance > matrix_max_distance)
        throw std::runtime_error("Path distance exceeds the max distance limit.");
    }
  }
}

namespace valhalla {
  namespace loki {

    worker_t::result_t loki_worker_t::optimized(boost::property_tree::ptree& request, http_request_t::info_t& request_info) {
      auto action_str = "many_to_many";
      //check that location size does not exceed max many-to-many.
      check_locations(locations.size(), max_locations.find(action_str)->second);
      for(size_t i = 0; i < locations.size()-1; ++i)
        check_distance(reader,locations,i,(i+1),locations.size(),max_distance.find(action_str)->second);

      auto max_location_distance = 0.f;
        for(size_t i = 0; i < max_manytomany_list.size(); ++i) {
          if (i==0)
            max_location_distance = max_manytomany_list[0];

          if (max_manytomany_list[i] >= max_location_distance)
            max_location_distance = max_manytomany_list[i];
        }
        valhalla::midgard::logging::Log("max_location_distance (km)::" + std::to_string(max_location_distance * kKmPerMeter), " [ANALYTICS] ");

      //correlate the various locations to the underlying graph
      for(size_t i = 0; i < locations.size(); ++i) {
        auto correlated = loki::Search(locations[i], reader, costing_filter);
        request.put_child("correlated_" + std::to_string(i), correlated.ToPtree(i));
      }

      //pass on to thor with type of matrix
      request.put("optimized", true);
      std::stringstream stream;
      boost::property_tree::write_info(stream, request);
      worker_t::result_t result{true};
      result.messages.emplace_back(stream.str());
      return result;
    }
  }
}