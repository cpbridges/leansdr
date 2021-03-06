// leandvbtx.cc copyright (c) 2016-2017 pabr@pabr.org
// http://www.pabr.org/radio/leandvb

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include "leansdr/framework.h"
#include "leansdr/generic.h"
#include "leansdr/dsp.h"
#include "leansdr/sdr.h"
#include "leansdr/dvb.h"
#include "leansdr/rs.h"
#include "leansdr/filtergen.h"

using namespace leansdr;

template<typename T>
struct interpolator : runnable {
  unsigned int d;

  interpolator(scheduler *sch, int _d, pipebuf<T> &_in, pipebuf<T> &_out)
    : runnable(sch, "interpolator"),
      d(_d),
      in(_in), out(_out, d) { }
  void run() {
    unsigned long count = min(in.readable(), out.writable()/d);
    T *pin=in.rd(), *pend=pin+count, *pout=out.wr();
    for ( ; pin<pend; ++pin ) {
      *pout++ = *pin;
      for ( int skip=d-1; skip--; ) *pout++ = 0;
    }
    in.read(count);
    out.written(count*d);
  }
private:
  pipereader<T> in;
  pipewriter<T> out;
};

struct config {
  float power;
  bool agc;
  int interp;
  int decim;
  float rolloff;
  bool verbose, debug;
  config()
    : power(1.0), agc(false),
      interp(2), decim(1), rolloff(0.35),
    verbose(false), debug(false)
  { }
};
    
void run(config &cfg) {
  scheduler sch;
  sch.verbose = cfg.verbose;
  sch.debug = cfg.debug;

  unsigned long BUF_PACKETS = 12;  // TBD Reduce copying
  unsigned long BUF_BYTES = SIZE_RSPACKET*12;
  unsigned long BUF_SYMBOLS = BUF_BYTES*8;
  unsigned long BUF_BASEBAND = 4096;

  // TS PACKETS ON STDIN

  pipebuf<tspacket> p_tspackets(&sch, "TS packets", BUF_PACKETS);
  file_reader<tspacket> r_stdin(&sch, 0, p_tspackets);

  // RANDOMIZER

  pipebuf<tspacket> p_rtspackets(&sch, "rand TS packets", BUF_PACKETS);
  randomizer r_rand(&sch, p_tspackets, p_rtspackets);

  // RS-ENCODER

  pipebuf< rspacket<u8> > p_rspackets(&sch, "RS-enc packets", BUF_PACKETS);
  rs_encoder r_rsenc(&sch, p_rtspackets, p_rspackets);

  // INTERLEAVER

  pipebuf<u8> p_mpegbytes(&sch, "mpegbytes", BUF_BYTES);
  interleaver r_inter(&sch, p_rspackets, p_mpegbytes);

  // CONVOLUTIONAL CODER

  pipebuf<u8> p_symbols(&sch, "symbols", BUF_SYMBOLS);
  dvb_convol r_convol(&sch, p_mpegbytes, p_symbols);

  // IQ MAPPER

  pipebuf<cf32> p_iqsymbols(&sch, "IQ symbols", BUF_SYMBOLS);
  cstln_transmitter<f32,0> r_mod(&sch, p_symbols, p_iqsymbols);
  cstln_lut<256> qpsk(cstln_lut<256>::QPSK);
  r_mod.cstln = &qpsk;

  // RESAMPLER

  pipebuf<cf32> p_interp(&sch, "interpolated", BUF_BASEBAND);
  float Fm = 1.0 / cfg.interp;
  int order = cfg.interp * 10;
  float *coeffs;
  int ncoeffs = filtergen::root_raised_cosine
    (order, Fm, cfg.rolloff, &coeffs);
  // This yields the desired power level even without AGC.
  filtergen::normalize_power(ncoeffs, coeffs, cfg.power/cstln_amp);

  if ( sch.verbose )
    fprintf(stderr, "Interpolation: ratio %d/%d, rolloff %f, %d coeffs\n",
	    cfg.interp, cfg.decim, cfg.rolloff, ncoeffs);
  if ( sch.debug )
    filtergen::dump_filter("rrc", ncoeffs, coeffs);

  fir_resampler<cf32,float>
    r_resampler(&sch, ncoeffs, coeffs,
		p_iqsymbols, p_interp, cfg.interp, 1);

  pipebuf<cf32> p_resampled(&sch, "resampled", BUF_BASEBAND);
  decimator<cf32> r_decim(&sch, cfg.decim, p_interp, p_resampled);

  pipebuf<cf32> *tail = &p_resampled;

  // AGC

  if ( cfg.agc ) {
    pipebuf<cf32> *p_agc =
      new pipebuf<cf32>(&sch, "AGC", BUF_BASEBAND);
    simple_agc<f32> *r_agc =
      new simple_agc<f32>(&sch, *tail, *p_agc);
    r_agc->out_rms = cfg.power / sqrtf((float)cfg.interp/cfg.decim);
    // Adjust bandwidth for large interpolation ratios.
    r_agc->bw = 0.001 * cfg.decim / cfg.interp;
    tail = p_agc;
  }

  // IQ ON STDOUT

  file_writer<cf32> r_stdout(&sch, *tail, 1);

  sch.run();
  sch.shutdown();
  if ( sch.verbose ) sch.dump();
}

// Command-line

void usage(const char *name, FILE *f, int c) {
  fprintf(f, "Usage: %s [options]  < TS  > IQ\n", name);
  fprintf(f, "Modulate MPEG packets into a DVB-S baseband signal\n");
  fprintf(f, "Output float complex samples\n");
  fprintf
    (f, "\nOptions:"
     "  -f INTERP[/DECIM]        Samples per symbols (default: 2)\n"
     "  --roll-off R             RRC roll-off (defalt: 0.35)\n"
     "  --power P                Output power (dB)\n"
     "  --agc                    Better regulation of output power\n"
     "  -v                       Verbose output\n"
     );
  exit(c);
}

int main(int argc, char *argv[]) {
  config cfg;

  for ( int i=1; i<argc; ++i ) {
    if      ( ! strcmp(argv[i], "-h") )
      usage(argv[0], stdout, 0);
    else if ( ! strcmp(argv[i], "-v") )
      cfg.verbose = true;
    else if ( ! strcmp(argv[i], "-d") )
      cfg.debug = true;
    else if ( ! strcmp(argv[i], "-f") && i+1<argc ) {
      ++i;
      cfg.decim = 1;
      if ( sscanf(argv[i], "%d/%d", &cfg.interp, &cfg.decim) < 1 )
	usage(argv[0], stderr, 1);
    }
    else if ( ! strcmp(argv[i], "--roll-off") && i+1<argc )
      cfg.rolloff = atof(argv[++i]);
    else if ( ! strcmp(argv[i], "--power") && i+1<argc )
      cfg.power = expf(logf(10)*atof(argv[++i])/20);
    else if ( ! strcmp(argv[i], "--agc") )
      cfg.agc = true;
    else 
      usage(argv[0], stderr, 1);
  }

  run(cfg);

  return 0;
}
