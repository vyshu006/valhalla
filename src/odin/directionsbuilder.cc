#include "odin/directionsbuilder.h"
#include "exceptions.h"
#include "odin/enhancedtrippath.h"
#include "odin/maneuversbuilder.h"
#include "odin/markup_formatter.h"
#include "odin/narrative_builder_factory.h"
#include "odin/narrativebuilder.h"
#include "proto/directions.pb.h"
#include "proto/options.pb.h"

namespace {
// Minimum edge length to verify heading (~3 feet)
constexpr auto kMinEdgeLength = 0.001f;

} // namespace

namespace valhalla {
namespace odin {

// Returns the trip directions based on the specified directions options
// and trip path. This method calls ManeuversBuilder::Build and
// NarrativeBuilder::Build to form the maneuver list. This method
// calls PopulateDirectionsLeg to transform the maneuver list into the
// trip directions.
void DirectionsBuilder::Build(Api& api, const MarkupFormatter& markup_formatter) {
  const auto& options = api.options();
  for (auto& trip_route : *api.mutable_trip()->mutable_routes()) {
    auto& directions_route = *api.mutable_directions()->mutable_routes()->Add();
    for (auto& trip_path : *trip_route.mutable_legs()) {
      auto& trip_directions = *directions_route.mutable_legs()->Add();

      // Validate trip path node list
      if (trip_path.node_size() < 1) {
        throw valhalla_exception_t{210};
      }

      // Create an enhanced trip path from the specified trip_path
      EnhancedTripLeg etp(trip_path);

      // Produce maneuvers if desired
      std::list<Maneuver> maneuvers;
      if (options.directions_type() != DirectionsType::none) {
        // Update the heading of ~0 length edges
        UpdateHeading(&etp);

        ManeuversBuilder maneuversBuilder(options, &etp);
        maneuvers = maneuversBuilder.Build();

        // Create the instructions if desired
        if (options.directions_type() == DirectionsType::instructions) {
          std::unique_ptr<NarrativeBuilder> narrative_builder =
              NarrativeBuilderFactory::Create(options, &etp, markup_formatter);
          narrative_builder->Build(maneuvers);
        }
      }

      // Return trip directions
      PopulateDirectionsLeg(options, &etp, maneuvers, trip_directions);
    }
  }
}

// Update the heading of ~0 length edges.
void DirectionsBuilder::UpdateHeading(EnhancedTripLeg* etp) {

  for (int x = 0; x < etp->node_size(); ++x) {
    auto prev_edge = etp->GetPrevEdge(x);
    auto curr_edge = etp->GetCurrEdge(x);
    auto next_edge = etp->GetNextEdge(x);

    // If very short edge and no headings
    if (curr_edge && (curr_edge->length_km() <= kMinEdgeLength) &&
        (curr_edge->begin_heading() == 0) && (curr_edge->end_heading() == 0)) {
      // Use next edge to set the current begin/end heading
      if (next_edge && (next_edge->length_km() > kMinEdgeLength)) {
        curr_edge->set_begin_heading(next_edge->begin_heading());
        curr_edge->set_end_heading(next_edge->begin_heading());
      }
      // Use prev edge to set the current begin/end heading
      else if (prev_edge && (prev_edge->length_km() > kMinEdgeLength)) {
        curr_edge->set_begin_heading(prev_edge->end_heading());
        curr_edge->set_end_heading(prev_edge->end_heading());
      }
    }
  }
}

// Returns the trip directions based on the specified directions options,
// trip path, and maneuver list.
void DirectionsBuilder::PopulateDirectionsLeg(const Options& options,
                                              EnhancedTripLeg* etp,
                                              std::list<Maneuver>& maneuvers,
                                              DirectionsLeg& trip_directions) {
  // 1. Populate basic metadata
  trip_directions.set_trip_id(etp->trip_id());
  trip_directions.set_leg_id(etp->leg_id());
  trip_directions.set_leg_count(etp->leg_count());
  trip_directions.mutable_level_changes()->CopyFrom(etp->level_changes());
  trip_directions.mutable_location()->CopyFrom(etp->location());

  // 2. THE MAIN LOOP: Process each maneuver exactly ONCE
  for (const auto& maneuver : maneuvers) {
    auto* trip_maneuver = trip_directions.add_maneuver();
    trip_maneuver->set_type(maneuver.type());
    trip_maneuver->set_text_instruction(maneuver.instruction());
    trip_maneuver->set_begin_path_index(maneuver.begin_node_index());
    trip_maneuver->set_end_path_index(maneuver.end_node_index());

    // --- GSOC 2026: DYNAMIC LANDMARK INJECTION ---
    if (!maneuver.landmarks().empty()) {
      std::string landmark_name = maneuver.landmarks().front().name();
      if (!landmark_name.empty()) {
        // Update both text and verbal (voice) instructions
        std::string updated_instr = trip_maneuver->text_instruction() + " after " + landmark_name;
        trip_maneuver->set_text_instruction(updated_instr);
        trip_maneuver->set_verbal_pre_transition_instruction(updated_instr);
        
        std::cout << "[ODIN] Successfully injected: " << landmark_name << std::endl;
      }
    }

    // 3. Set street names
    for (const auto& street_name : maneuver.street_names()) {
      auto* maneuver_street_name = trip_maneuver->add_street_name();
      maneuver_street_name->set_value(street_name->value());
      maneuver_street_name->set_is_route_number(street_name->is_route_number());
    }

    // 4. Set maneuver technical details
    trip_maneuver->set_length(maneuver.length(options.units()));
    trip_maneuver->set_time(maneuver.time());
    trip_maneuver->set_begin_cardinal_direction(maneuver.begin_cardinal_direction());
    trip_maneuver->set_begin_heading(maneuver.begin_heading());
    trip_maneuver->set_turn_degree(maneuver.turn_degree());
    trip_maneuver->set_begin_shape_index(maneuver.begin_shape_index());
    trip_maneuver->set_end_shape_index(maneuver.end_shape_index());

    // 5. Finalize landmarks for the response
    for (const auto& l : maneuver.landmarks()) {
      trip_maneuver->add_landmarks()->CopyFrom(l);
    }
  } 

  // 6. Final Summary and Shape
  trip_directions.mutable_summary()->set_length(etp->GetLength(options.units()));
  trip_directions.mutable_summary()->set_time(
      etp->node(etp->GetLastNodeIndex()).cost().elapsed_cost().seconds());
  
  auto* mutable_bbox = trip_directions.mutable_summary()->mutable_bbox();
  mutable_bbox->mutable_min_ll()->set_lat(etp->bbox().min_ll().lat());
  mutable_bbox->mutable_min_ll()->set_lng(etp->bbox().min_ll().lng());
  mutable_bbox->mutable_max_ll()->set_lat(etp->bbox().max_ll().lat());
  mutable_bbox->mutable_max_ll()->set_lng(etp->bbox().max_ll().lng());

  trip_directions.set_shape(etp->shape());
  
  // FINAL EMERGENCY SENSOR - GSOC 2026 (Verify injection in logs)
  for (const auto& m : trip_directions.maneuver()) {
    for (const auto& l : m.landmarks()) {
      std::cout << "\n[SUCCESS] MANEUVER LANDMARK DETECTED: " << l.name() << " !!!\n" << std::endl;
    }
  }
}

} // namespace odin
} // namespace valhalla