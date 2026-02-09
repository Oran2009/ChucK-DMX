//--------------------------------------------------------------------
// DMX ChuGin Test
//--------------------------------------------------------------------
// 1) try running this program after building your chugin
//    (the build process should yield a DMX.chug file)
//
// 2) you can manually load the chugin when you run this program
//    `chuck --chugin:x64/Debug/DMX.chug DMX-test.ck`
//
// 3) OR you can put the chugin into your chugins search path
//    NOTE: not recommended until you feel the chugin to be
//    reasonably stable, as chugins in your chugins search paths
//    will automatically load every time you run `chuck` or
//    start the VM in miniAudicle...
//
// Want to see more information? Add the --verbose:3 (-v3) flag:
//    `chuck --chugin:DMX.chug DMX-test.ck -v3`
//--------------------------------------------------------------------

// List available serial ports
<<< "Available serial ports:", DMX.ports() >>>;

// Setup
DMX dmx;
DMX.SACN => dmx.protocol;    // use protocol constants
1 => dmx.universe;           // universe 1 is the active universe
"My ChucK Show" => dmx.name; // source name visible in sACN/ArtNet
44 => dmx.rate;
// "COM5" => dmx.port;       // uncomment for serial protocols
// 150 => dmx.priority;      // sACN priority (0-200, default 100)

// Add a second universe for multi-universe output
dmx.addUniverse(2);

// Initialize and check connection (inits all configured universes)
if (!dmx.init()) {
    <<< "DMX init failed!" >>>;
    me.exit();
}
<<< "DMX connected:", dmx.connected() >>>;
<<< "Source name:", dmx.name() >>>;

// Start background send thread (handles sending + fade interpolation)
dmx.start();

1 => int baseAddr;

// Audio setup
SinOsc osc => dac;
0.05 => float baseGain;

// Color palette: R, G, B, NW, WW
[
    [255, 0, 0, 0, 0],
    [0, 255, 0, 0, 0],
    [0, 0, 255, 0, 0],
    [255, 255, 0, 0, 0],
    [0, 255, 255, 0, 0],
    [255, 0, 255, 0, 0],
    [255, 255, 255, 0, 0],
    [255, 255, 255, 128, 128]
] @=> int colors[][];

fun void rampColorsAndTone() {
    0 => int idx;
    0.0 => float dim;
    0.02 => float step;
    true => int rampUp;
    true => int rampPhase; // true: ramping, false: sudden jumps
    8::second => dur phaseDuration;
    0::second => dur elapsed;

    while (true) {
        if (rampPhase) {
            // Smooth ramp using fades
            if (rampUp) step +=> dim; else step -=> dim;

            if (dim > 1.0) {
                1.0 => dim;
                false => rampUp;
            } else if (dim < 0.0) {
                0.0 => dim;
                true => rampUp;
                (idx + 1) % colors.size() => idx;
            }

            colors[idx] @=> int currColor[];

            // Set DMX on universe 1 using batch channels
            [
                (currColor[0] * dim) $ int,
                (currColor[1] * dim) $ int,
                (currColor[2] * dim) $ int,
                (currColor[3] * dim) $ int,
                (currColor[4] * dim) $ int
            ] @=> int scaled[];

            1 => dmx.universe;           // target universe 1
            dmx.channels(baseAddr, scaled);

            // Mirror inverted colors on universe 2
            [
                (currColor[2] * dim) $ int,
                (currColor[1] * dim) $ int,
                (currColor[0] * dim) $ int,
                (currColor[4] * dim) $ int,
                (currColor[3] * dim) $ int
            ] @=> int inverted[];

            2 => dmx.universe;           // target universe 2
            dmx.channels(baseAddr, inverted);

            // Background thread handles sending all universes

            // Smooth sine frequency sweep synced to ramp dim
            220.0 + dim * 880.0 => float freq;
            osc.freq(freq);

            // Smooth gain modulation
            baseGain + 0.5 * dim => float g;
            osc.gain(g);

            30::ms => now;
            30::ms +=> elapsed;
        } else {
            // Sudden jumps phase - use fade() for smooth color transitions
            colors[idx] @=> int currColor[];

            // Fade channels on universe 1
            1 => dmx.universe;
            for (int i; i < 5; i++) {
                dmx.fade(baseAddr + i, currColor[i], 100);
            }

            // Fade complementary colors on universe 2
            2 => dmx.universe;
            dmx.fade(baseAddr, currColor[2], 100);
            dmx.fade(baseAddr + 1, currColor[1], 100);
            dmx.fade(baseAddr + 2, currColor[0], 100);
            dmx.fade(baseAddr + 3, currColor[4], 100);
            dmx.fade(baseAddr + 4, currColor[3], 100);

            // Sharp staccato tone: short burst at higher freq
            880 + 660 * (idx % 2) => float freq;
            osc.freq(freq);
            1.0 => osc.gain;
            80::ms => now;
            0.0 => osc.gain;

            // Pause silent till next burst
            420::ms => now;

            (idx + 1) % colors.size() => idx;
            500::ms => now; // accumulate duration
            500::ms +=> elapsed;
        }

        // Switch phase every phaseDuration
        if (elapsed >= phaseDuration) {
            !rampPhase => rampPhase;
            0::second => elapsed;
            // Reset ramp variables if switching to ramp
            if (rampPhase) {
                0.0 => dim;
                true => rampUp;
            }
        }
    }
}

fun void play() {
    rampColorsAndTone();
}

spork ~ play();

// Keep the VM alive
while (true) {
    25::ms => now;
}
