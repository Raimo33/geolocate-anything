// `geoloc aircraft ...`: the aircraft-Doppler commands, registered on the CLI app.
#pragma once

namespace CLI {
class App;
}

namespace geo::air {

// Adds the `aircraft` subcommand tree; `rc` receives the chosen command's exit code.
void register_commands(CLI::App& app, int& rc);

}  // namespace geo::air
