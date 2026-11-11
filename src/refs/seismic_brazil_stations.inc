// Brazil's SIN from the University of São Paulo's archive (docs/grids/seismic_brazil.md): 100 samples/s
// short-period channels of one recorder, the 60 Hz line folded to 40 Hz. id, lat, lon, epoch from, to
// (exclusive), {year, share of that year's screened hours in which the hum agreed with the other channel at
// r >= 0.99 after 12 s smoothing}. Coordinates are the sensors' sites; the hum is picked up at the recorder.
// BL.VAO3..EHZ is left out: it carries the same signal as BL.CAM1 (waveforms r 0.94-1.00), so it confirms nothing.
const std::vector<Station> STATIONS = {
    {"BL.CAM1..EHZ", -22.39778, -41.83375, "2004-02-01", "2008-09-01", {{2004, 1.00f}, {2005, 1.00f}, {2006, 1.00f}, {2007, 1.00f}, {2008, 1.00f}}},
    {"BL.CAM4..EHZ", -22.3568, -42.5653, "2004-02-01", "2008-09-01", {{2004, 0.50f}, {2005, 1.00f}, {2006, 1.00f}, {2007, 0.50f}, {2008, 0.00f}}},
};
