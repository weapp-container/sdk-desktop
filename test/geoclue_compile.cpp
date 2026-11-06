// Compile the actual GIO transport without requiring GTK or a GeoClue daemon.
#include "device_location_geoclue.hpp"
void compile_geoclue_location_source() {
  (void)&weapp::desktop::detail::geo_location_factory;
}
