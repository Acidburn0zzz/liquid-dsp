/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010, 2011 Virginia Polytechnic
 *                                      Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// Symbol synchronizer
//
// References:
//  [Mengali:1997] Umberto Mengali and ALdo N. D'Andrea,
//      "Synchronization Techniques for Digital Receivers,"
//      Plenum Press, New York & London, 1997.
//  [harris:2001] frederic j. harris and Michael Rice,
//      "Multirate Digital Filters for Symbol Timing Synchronization
//      in Software Defined Radios," IEEE Journal on Selected Areas
//      of Communications, vol. 19, no. 12, December, 2001, pp.
//      2346-2357.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// use theoretical 2nd-order integrating PLL filter?
// NOTE: works ok for k_out=[1,2]
#define SYMSYNC_USE_PLL         0

#define DEBUG_SYMSYNC           0
#define DEBUG_SYMSYNC_PRINT     0
#define DEBUG_SYMSYNC_FILENAME  "symsync_internal_debug.m"
#define DEBUG_BUFFER_LEN        (1024)

// defined:
//  TO          output data type
//  TC          coefficient data type
//  TI          input data type
//  SYMSYNC()    name-mangling macro
//  FIRPFB()    firpfb macro

struct SYMSYNC(_s) {
    unsigned int h_len;         // matched filter length
    unsigned int k;             // samples/symbol (input)
    unsigned int k_out;         // samples/symbol (output)

    unsigned int decim_counter; // decimation counter
    int is_locked;              // synchronizer locked flag

    float r;                    // rate
    int b;                      // filterbank index
    float del;                  // fractional delay step

    // floating-point phase
    float tau;                  // accumulated timing phase (0 <= tau <= 1)
    float bf;                   // soft filterbank index

    // loop filter
    float alpha;                // percent of old error sample to retain
    float beta;                 // percent of new error sample to retain
    float q;                    // instantaneous timing error estimate
    float q_hat;                // filtered timing error estimate
    float q_prime;              // buffered timing error estimate
#if SYMSYNC_USE_PLL
    // phase-locked loop
    float B[3];
    float A[3];
    iirfiltsos_rrrf pll;
#endif

    unsigned int npfb;
    FIRPFB()  mf;   // matched filter
    FIRPFB() dmf;   // derivative matched filter

#if DEBUG_SYMSYNC
    windowf debug_del;
    windowf debug_tau;
    windowf debug_bsoft;
    windowf debug_b;
    windowf debug_q_hat;
#endif
};

// create synchronizer object
//
//  _k      :   samples per symbol
//  _npfb   :   number of filters in the bank
//  _h      :   matched filter coefficients
//  _h_len  :   length of matched filter
SYMSYNC() SYMSYNC(_create)(unsigned int _k,
                           unsigned int _npfb,
                           TC * _h,
                           unsigned int _h_len)
{
    // validate input
    if (_k < 2) {
        fprintf(stderr,"error: symsync_xxxt_create(), input sample rate must be at least 2\n");
        exit(1);
    } else if (_h_len == 0) {
        fprintf(stderr,"error: symsync_xxxt_create(), filter length must be greater than zero\n");
        exit(1);
    } else if (_npfb == 0) {
        fprintf(stderr,"error: symsync_xxxt_create(), number of filter banks must be greater than zero\n");
        exit(1);
    }

    SYMSYNC() q = (SYMSYNC()) malloc(sizeof(struct SYMSYNC(_s)));
    q->k = _k;

    q->npfb = _npfb;
    q->tau = 0.0f;
    q->bf  = 0.0f;
    q->b   = 0;

    // set output rate (nominally 1, full decimation)
    SYMSYNC(_set_output_rate)(q, 1);

    // TODO: validate length
    q->h_len = (_h_len-1)/q->npfb;
    
    // compute derivative filter
    TC dh[_h_len];
    unsigned int i;
    for (i=0; i<_h_len; i++) {
        if (i==0) {
            dh[i] = _h[i+1] - _h[_h_len-1];
        } else if (i==_h_len-1) {
            dh[i] = _h[0]   - _h[i-1];
        } else {
            dh[i] = _h[i+1] - _h[i-1];
        }

        // apply scaling factor (derivative approximation is
        // scaled by the number of filters in the bank)
        dh[i] *= (float)_npfb / 16.0f;
    }
    q->mf  = FIRPFB(_create)(q->npfb, _h, _h_len);
    q->dmf = FIRPFB(_create)(q->npfb, dh, _h_len);

    // reset state and initialize loop filter
#if SYMSYNC_USE_PLL
    q->A[0] = 1.0f;     q->B[0] = 0.0f;
    q->A[1] = 0.0f;     q->B[1] = 0.0f;
    q->A[2] = 0.0f;     q->B[2] = 0.0f;
    q->pll = iirfiltsos_rrrf_create(q->B, q->A);
#endif
    SYMSYNC(_clear)(q);
    SYMSYNC(_set_lf_bw)(q, 0.01f);

    // set output rate nominally at 1 sample/symbol (full decimation)
    SYMSYNC(_set_output_rate)(q, 1);

    // unlock loop control
    SYMSYNC(_unlock)(q);

#if DEBUG_SYMSYNC
    q->debug_del   = windowf_create(DEBUG_BUFFER_LEN);
    q->debug_tau   = windowf_create(DEBUG_BUFFER_LEN);
    q->debug_bsoft = windowf_create(DEBUG_BUFFER_LEN);
    q->debug_b     = windowf_create(DEBUG_BUFFER_LEN);
    q->debug_q_hat = windowf_create(DEBUG_BUFFER_LEN);
#endif

    // return main object
    return q;
}

// create square-root Nyquist symbol synchronizer
//  _type   : filter type (e.g. LIQUID_RNYQUIST_RRC)
//  _k      : samples/symbol
//  _m      : symbol delay
//  _beta   : rolloff factor (0 < beta <= 1)
//  _npfb   : number of filters in the bank
SYMSYNC() SYMSYNC(_create_rnyquist)(int _type,
                                    unsigned int _k,
                                    unsigned int _m,
                                    float _beta,
                                    unsigned int _npfb)
{
    // validate input
    if (_k < 2) {
        fprintf(stderr,"error: symsync_xxxt_create_rnyquist(), samples/symbol must be at least 2\n");
        exit(1);
    } else if (_m == 0) {
        fprintf(stderr,"error: symsync_xxxt_create_rnyquist(), filter delay (m) must be greater than zero\n");
        exit(1);
    } else if (_beta < 0.0f || _beta > 1.0f) {
        fprintf(stderr,"error: symsync_xxxt_create_rnyquist(), filter excess bandwidth must be in [0,1]\n");
        exit(1);
    }

    // allocate memory for filter coefficients
    unsigned int H_len = 2*_npfb*_k*_m + 1;
    float Hf[H_len];

    // design square-root Nyquist pulse-shaping filter
    design_rnyquist_filter(_type, _k*_npfb, _m, _beta, 0, Hf);

    // copy coefficients to type-specific array
    TC H[H_len];
    unsigned int i;
    for (i=0; i<H_len; i++)
        H[i] = Hf[i];

    // create object and return
    return SYMSYNC(_create)(_k, _npfb, H, H_len);
}

void SYMSYNC(_destroy)(SYMSYNC() _q)
{
#if DEBUG_SYMSYNC
    // output debugging file
    SYMSYNC(_output_debug_file)(_q, DEBUG_SYMSYNC_FILENAME);

    // destroy internal window objects
    windowf_destroy(_q->debug_del);
    windowf_destroy(_q->debug_tau);
    windowf_destroy(_q->debug_bsoft);
    windowf_destroy(_q->debug_b);
    windowf_destroy(_q->debug_q_hat);
#endif

    // destroy filterbank objects
    FIRPFB(_destroy)(_q->mf);
    FIRPFB(_destroy)(_q->dmf);

#if SYMSYNC_USE_PLL
    iirfiltsos_rrrf_destroy(_q->pll);
#endif

    // free main object memory
    free(_q);
}

void SYMSYNC(_print)(SYMSYNC() _q)
{
    printf("symsync [rate: %f]\n", _q->r);
    FIRPFB(_print)(_q->mf);
}

void SYMSYNC(_reset)(SYMSYNC() _q)
{
    FIRPFB(_clear)(_q->mf);

    _q->b       = 0;
    _q->tau     = 0.0f;
    _q->bf      = 0.0f;
    _q->q       = 0.0f;
    _q->q_hat   = 0.0f;
    _q->q_prime = 0.0f;
    _q->decim_counter = 0;
#if SYMSYNC_USE_PLL
    iirfiltsos_rrrf_clear(_q->pll);
#endif
}

void SYMSYNC(_clear)(SYMSYNC() _q) {
    SYMSYNC(_reset)(_q);
}

// lock synchronizer object
void SYMSYNC(_lock)(SYMSYNC() _q)
{
    _q->is_locked = 1;
}

// unlock synchronizer object
void SYMSYNC(_unlock)(SYMSYNC() _q)
{
    _q->is_locked = 0;
}

// set rate
void SYMSYNC(_setrate)(SYMSYNC() _q, float _rate)
{
    // TODO : validate rate, validate this method
    _q->r = _rate;
    _q->del = 1.0f / _q->r;

}

// set synchronizer loop filter bandwidth
//  _q      :   synchronizer object
//  _bt     :   bandwidth
void SYMSYNC(_set_lf_bw)(SYMSYNC() _q,
                         float _bt)
{
    // validate input
    if (_bt < 0.0f || _bt > 1.0f) {
        fprintf(stderr,"error: symsync_xxxt_set_lf_bt(), bandwidth must be in [0,1]\n");
        exit(1);
    }

#if SYMSYNC_USE_PLL
    float zeta = 1.1f;
    float K = 1000.0f;
    iirdes_pll_active_lag(0.5f*_bt, zeta, K, _q->B, _q->A);
    iirfiltsos_rrrf_set_coefficients(_q->pll, _q->B, _q->A);
#else
    // set loop filter bandwidth
    _q->alpha = 1.00f - _bt;    // percent of old sample to retain
    _q->beta  = 0.22f * _bt;    // percent of new sample to retain
#endif
}

// set synchronizer output rate (samples/symbol)
//  _q      :   synchronizer object
//  _k_out  :   output samples/symbol
void SYMSYNC(_set_output_rate)(SYMSYNC() _q,
                               unsigned int _k_out)
{
    // validate input
    if (_k_out == 0) {
        fprintf(stderr,"error: symsync_xxxt_output_rate(), output rate must be greater than 0\n");
        exit(1);
    }

    // set output rate
    _q->k_out = _k_out;

    _q->r   = (float)_q->k_out / (float)_q->k;
    _q->del = 1.0f / _q->r;
}

float SYMSYNC(_get_tau)(SYMSYNC() _q)
{
    return _q->tau;
}

// execute synchronizer on input data array
//  _q      :   synchronizer object
//  _x      :   input data array
//  _nx     :   number of input samples
//  _y      :   output data array
//  _ny     :   number of samples written to output buffer
void SYMSYNC(_execute)(SYMSYNC() _q,
                       TI * _x,
                       unsigned int _nx,
                       TO * _y,
                       unsigned int *_ny)
{
    unsigned int i, ny=0, k=0;
    for (i=0; i<_nx; i++) {
        SYMSYNC(_step)(_q, _x[i], &_y[ny], &k);
        ny += k;
        //printf("%u\n",k);
    }
    *_ny = ny;
}

// advance synchronizer's internal loop filter
//  _q      :   synchronizer object
//  _mf     :   matched-filter output
//  _dmf    :   derivative matched-filter output
void SYMSYNC(_advance_internal_loop)(SYMSYNC() _q,
                                     TO _mf,
                                     TO _dmf)
{
    //  1.  compute timing error signal, clipping large levels
#if 0
    _q->q = crealf(_mf)*crealf(_dmf) + cimagf(_mf)*cimagf(_dmf);
#else
    // TODO : use more efficient method to compute this
    _q->q = crealf( conjf(_mf)*_dmf );  // [Mengali:1997] Eq.~(8.3.5)
#endif
    if (_q->q > 1.0f)       _q->q =  1.0f;
    else if (_q->q < -1.0f) _q->q = -1.0f;

    //  2.  filter error signal: retain large percent (alpha) of
    //      old estimate and small percent (beta) of new estimate
#if SYMSYNC_USE_PLL
    iirfiltsos_rrrf_execute(_q->pll, _q->q, &_q->q_hat);
    _q->del = (float)(_q->k)/(float)(_q->k_out) + _q->q_hat;
    //_q->del = (float)(_q->k) * (1 + _q->q_hat);
#else
    _q->q_hat   = (_q->q)*(_q->beta) + (_q->q_prime)*(_q->alpha);
    _q->q_prime = _q->q_hat;

    // TODO : check the output step size, relative to input
    //_q->del     = (float)(_q->k) + _q->q_hat;
    _q->del     = (float)(_q->k)/(float)(_q->k_out) + _q->q_hat;
#endif

#if DEBUG_SYMSYNC_PRINT
    printf("q : %12.8f, del : %12.8f, q_hat : %12.8f\n", _q->q, _q->del, _q->q_hat);
#endif
}

void SYMSYNC(_step)(SYMSYNC() _q,
                    TI _x,
                    TO * _y,
                    unsigned int *_num_written)
{
    // push sample into MF and dMF filterbanks
    FIRPFB(_push)(_q->mf,  _x);
    FIRPFB(_push)(_q->dmf, _x);
    
    // matched and derivative matched-filter outputs
    TO  mf;
    TO dmf;

    unsigned int n=0;
    
    //while (_q->tau < 1.0f) {
    while (_q->b < _q->npfb) {

#if DEBUG_SYMSYNC_PRINT
        printf("  [%2u] : tau : %12.8f, b : %4u (%12.8f)\n", n, _q->tau, _q->b, _q->bf);
#endif

        // compute filterbank output
        FIRPFB(_execute)(_q->mf, _q->b, &mf);

        // scale output by samples/symbol
        _y[n] = mf / (float)(_q->k);

        // check output count and determine if this is 'ideal' timing output
        if (_q->decim_counter == _q->k_out) {
            // reset counter
            _q->decim_counter = 0;

#if DEBUG_SYMSYNC
        // save debugging variables
        windowf_push(_q->debug_del,    _q->del);
        windowf_push(_q->debug_tau,    _q->tau);
        windowf_push(_q->debug_bsoft,  _q->bf);
        windowf_push(_q->debug_b,      _q->b);
        windowf_push(_q->debug_q_hat,  _q->q_hat);
#endif

            if (_q->is_locked) continue;

            // compute dMF output
            FIRPFB(_execute)(_q->dmf, _q->b, &dmf);
            
            // update internal state
            SYMSYNC(_advance_internal_loop)(_q, mf, dmf);
        }
        _q->decim_counter++;

#if SYMSYNC_USE_PLL
        _q->tau     = _q->del;
#else
        _q->tau += _q->del;
#endif
        _q->bf = _q->tau * (float)(_q->npfb);
        _q->b  = (int)roundf(_q->bf);
        n++;
    }

    _q->tau -= 1.0f;
    _q->bf  -= (float)(_q->npfb);
    _q->b   -= _q->npfb;

    *_num_written = n;
}

// print results to output debugging file
void SYMSYNC(_output_debug_file)(SYMSYNC() _q,
                                 const char * _filename)
{
    FILE * fid = fopen(_filename, "w");
    if (!fid) {
        fprintf(stderr,"error: symsync_xxxt_output_debug_file(), could not open '%s' for writing\n", _filename);
        return;
    }
    fprintf(fid,"%% %s, auto-generated file\n\n", DEBUG_SYMSYNC_FILENAME);

    fprintf(fid,"npfb = %u;\n",_q->npfb);
    fprintf(fid,"k = %u;\n",_q->k);
    fprintf(fid,"\n\n");

    fprintf(fid,"alpha = %12.5e;\n",_q->alpha);
    fprintf(fid,"beta = %12.5e;\n",_q->beta);
    fprintf(fid,"\n\n");

#if DEBUG_SYMSYNC
    fprintf(fid,"n = %u;\n", DEBUG_BUFFER_LEN);
    float * r;
    unsigned int i;

    // print del buffer
    fprintf(fid,"del = zeros(1,n);\n");
    windowf_read(_q->debug_del, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"del(%4u) = %12.8f;\n", i+1, r[i]);
    fprintf(fid,"\n\n");

    // print tau buffer
    fprintf(fid,"tau = zeros(1,n);\n");
    windowf_read(_q->debug_tau, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"tau(%4u) = %12.8f;\n", i+1, r[i]);
    fprintf(fid,"\n\n");

    // print bsoft buffer
    fprintf(fid,"bf = zeros(1,n);\n");
    windowf_read(_q->debug_bsoft, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"bf(%4u) = %12.8f;\n", i+1, r[i]);
    fprintf(fid,"\n\n");

    // print b (filterbank index) buffer
    fprintf(fid,"b = zeros(1,n);\n");
    windowf_read(_q->debug_b, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"b(%4u) = %12.8f;\n", i+1, r[i]);
    fprintf(fid,"\n\n");

    // print filtered error signal
    fprintf(fid,"q_hat = zeros(1,n);\n");
    windowf_read(_q->debug_q_hat, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"q_hat(%4u) = %12.8f;\n", i+1, r[i]);
    fprintf(fid,"\n\n");

    fprintf(fid,"\n\n");
    fprintf(fid,"t=1:n;\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"hold on;\n");
    fprintf(fid,"plot(t,b,'Color',[0.5 0.5 0.5]);\n");
    fprintf(fid,"plot(t,bf,'LineWidth',2,'Color',[0 0.25 0.5]);\n");
    fprintf(fid,"hold off;\n");
    fprintf(fid,"grid on;\n");
    fprintf(fid,"axis([t(1) t(end) -1 npfb]);\n");
    fprintf(fid,"legend('b','b (soft)',0);\n");
    fprintf(fid,"xlabel('Symbol Index')\n");
    fprintf(fid,"ylabel('Polyphase Filter Index')\n");
    fprintf(fid,"%% done.\n");
#endif

    fclose(fid);
    printf("symsync: internal results written to '%s'\n", _filename);
}


