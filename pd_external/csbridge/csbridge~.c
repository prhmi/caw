/* csbridge~ : runs a Csound CSD inside Pd and moves audio through named audio channels.
 *
 *   [csbridge~ file.csd <nIn> <nOut>]
 *
 *   signal inlet  k  ->  Csound audio channel "in<k+1>"   (read in Csound:  aIn chnget "in1")
 *   Csound audio channel "out<k+1>" -> signal outlet k    (write in Csound: chnset aSig, "out1")
 *
 *   messages: [event i 1 0 2 440( = score line, [chn name value( = control channel, [read file.csd(, [reset(
 *
 * Pd's block size and Csound's ksmps do not have to match (FIFOs in between, costs up to ksmps samples of latency).
 * The CSD must use the same sr as Pd. Put  -n  (no audio) is added automatically.
 */
#include "m_pd.h"
#include <csound/csound.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAXCH 32
static t_class *csbridge_class;

typedef struct _csbridge {
    t_object x_obj;
    t_float  x_f;                 /* main signal inlet scalar */
    t_inlet *x_in[MAXCH];
    t_outlet *x_out[MAXCH];
    int      nin, nout;
    CSOUND  *cs;
    int      ksmps, ready;
    MYFLT   *chin[MAXCH];         /* Csound channel buffers (ksmps long) */
    MYFLT   *chout[MAXCH];
    t_float *fin[MAXCH];          /* input FIFOs  */
    t_float *fout[MAXCH];         /* output FIFOs */
    int      nfin, nfout, cap;
    t_symbol *file;
    t_sample **sigv;              /* scratch for dsp */
} t_csbridge;

static void csbridge_free_cs(t_csbridge *x)
{
    int i;
    x->ready = 0;
    if (x->cs) { csoundStop(x->cs); csoundDestroy(x->cs); x->cs = NULL; }
    for (i = 0; i < MAXCH; i++) {
        if (x->fin[i])  { freebytes(x->fin[i],  x->cap * sizeof(t_float)); x->fin[i] = NULL; }
        if (x->fout[i]) { freebytes(x->fout[i], x->cap * sizeof(t_float)); x->fout[i] = NULL; }
        x->chin[i] = x->chout[i] = NULL;
    }
    x->nfin = x->nfout = 0;
}

static void csbridge_load(t_csbridge *x)
{
    char name[16];
    const char *argv[4];
    int i, err;
    csbridge_free_cs(x);
    if (!x->file || !*x->file->s_name) return;

    csoundInitialize(CSOUNDINIT_NO_ATEXIT | CSOUNDINIT_NO_SIGNAL_HANDLER);
    x->cs = csoundCreate(NULL);
    if (!x->cs) { pd_error(x, "csbridge~: csoundCreate failed"); return; }
    csoundSetMessageLevel(x->cs, 0);
    argv[0] = "csound"; argv[1] = "-n"; argv[2] = x->file->s_name; argv[3] = NULL;
    err = csoundCompile(x->cs, 3, argv);
    if (err) { pd_error(x, "csbridge~: could not compile %s", x->file->s_name); csbridge_free_cs(x); return; }

    if ((int)csoundGetSr(x->cs) != (int)sys_getsr())
        post("csbridge~ WARNING: CSD sr=%d but Pd sr=%d (set sr in the CSD to match)", (int)csoundGetSr(x->cs), (int)sys_getsr());
    x->ksmps = csoundGetKsmps(x->cs);
    x->cap = x->ksmps + 8192;
    for (i = 0; i < x->nin; i++) {
        sprintf(name, "in%d", i + 1);
        if (csoundGetChannelPtr(x->cs, &x->chin[i], name, CSOUND_AUDIO_CHANNEL | CSOUND_INPUT_CHANNEL) < 0)
            { pd_error(x, "csbridge~: channel %s failed", name); }
        x->fin[i] = (t_float *)getbytes(x->cap * sizeof(t_float));
    }
    for (i = 0; i < x->nout; i++) {
        sprintf(name, "out%d", i + 1);
        if (csoundGetChannelPtr(x->cs, &x->chout[i], name, CSOUND_AUDIO_CHANNEL | CSOUND_OUTPUT_CHANNEL) < 0)
            { pd_error(x, "csbridge~: channel %s failed", name); }
        x->fout[i] = (t_float *)getbytes(x->cap * sizeof(t_float));
    }
    x->nfin = 0;
    x->nfout = x->ksmps;          /* prefill silence so the first blocks never underrun */
    x->ready = 1;
    post("csbridge~: %s loaded (ksmps %d, %d in, %d out)", x->file->s_name, x->ksmps, x->nin, x->nout);
}

static t_int *csbridge_perform(t_int *w)
{
    t_csbridge *x = (t_csbridge *)w[1];
    int n = (int)w[2], i, k;
    t_sample **in  = x->sigv;
    t_sample **out = x->sigv + x->nin;

    if (!x->ready || x->nfin + n > x->cap || x->nfout + x->ksmps > x->cap) {
        for (k = 0; k < x->nout; k++) memset(out[k], 0, n * sizeof(t_sample));
        if (x->ready) { x->nfin = 0; x->nfout = x->ksmps; }   /* overflow: resync */
        return (w + 3);
    }
    for (i = 0; i < x->nin; i++) memcpy(x->fin[i] + x->nfin, in[i], n * sizeof(t_sample));
    x->nfin += n;

    while (x->nfin >= x->ksmps) {
        for (i = 0; i < x->nin; i++) {
            if (!x->chin[i]) continue;
            for (k = 0; k < x->ksmps; k++) x->chin[i][k] = (MYFLT)x->fin[i][k];
            memmove(x->fin[i], x->fin[i] + x->ksmps, (x->nfin - x->ksmps) * sizeof(t_float));
        }
        x->nfin -= x->ksmps;
        if (csoundPerformKsmps(x->cs) != 0) { x->ready = 0; break; }   /* score ended or error */
        for (i = 0; i < x->nout; i++) {
            if (!x->chout[i]) continue;
            for (k = 0; k < x->ksmps; k++) x->fout[i][x->nfout + k] = (t_float)x->chout[i][k];
        }
        x->nfout += x->ksmps;
    }
    for (i = 0; i < x->nout; i++) {
        if (x->nfout >= n) {
            memcpy(out[i], x->fout[i], n * sizeof(t_sample));
            memmove(x->fout[i], x->fout[i] + n, (x->nfout - n) * sizeof(t_float));
        } else memset(out[i], 0, n * sizeof(t_sample));
    }
    if (x->nfout >= n) x->nfout -= n;
    return (w + 3);
}

static void csbridge_dsp(t_csbridge *x, t_signal **sp)
{
    int i;
    for (i = 0; i < x->nin + x->nout; i++) x->sigv[i] = sp[i]->s_vec;
    dsp_add(csbridge_perform, 2, x, (t_int)sp[0]->s_n);
}

static void csbridge_read(t_csbridge *x, t_symbol *s) { x->file = s; csbridge_load(x); }
static void csbridge_reset(t_csbridge *x) { csbridge_load(x); }

static void csbridge_chn(t_csbridge *x, t_symbol *s, t_floatarg v)
{
    if (x->ready) csoundSetControlChannel(x->cs, s->s_name, (MYFLT)v);
}

static void csbridge_event(t_csbridge *x, t_symbol *s, int argc, t_atom *argv)
{
    MYFLT p[64]; char type; int i;
    (void)s;
    if (!x->ready || argc < 2 || argc > 65) return;
    type = argv[0].a_type == A_SYMBOL ? atom_getsymbol(argv)->s_name[0] : 'i';
    for (i = 1; i < argc; i++) p[i - 1] = (MYFLT)atom_getfloat(argv + i);
    csoundEvent(x->cs, type == 'f' ? CS_FUNCTION_EVENT : (type == 'e' ? CS_END_EVENT : CS_INSTR_EVENT), p, argc - 1, 0);
}

static void *csbridge_new(t_symbol *s, int argc, t_atom *argv)
{
    t_csbridge *x = (t_csbridge *)pd_new(csbridge_class);
    int i;
    (void)s;
    x->file = (argc > 0 && argv[0].a_type == A_SYMBOL) ? atom_getsymbol(argv) : NULL;
    x->nin  = argc > 1 ? (int)atom_getfloat(argv + 1) : 1;
    x->nout = argc > 2 ? (int)atom_getfloat(argv + 2) : 2;
    if (x->nin < 0) x->nin = 0;  if (x->nin > MAXCH) x->nin = MAXCH;
    if (x->nout < 0) x->nout = 0; if (x->nout > MAXCH) x->nout = MAXCH;
    x->sigv = (t_sample **)getbytes((x->nin + x->nout + 1) * sizeof(t_sample *));
    for (i = 1; i < x->nin; i++) x->x_in[i] = inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    for (i = 0; i < x->nout; i++) x->x_out[i] = outlet_new(&x->x_obj, &s_signal);
    if (x->nin == 0) { /* keep first inlet harmless */ }
    csbridge_load(x);
    return x;
}

static void csbridge_destroy(t_csbridge *x)
{
    csbridge_free_cs(x);
    freebytes(x->sigv, (x->nin + x->nout + 1) * sizeof(t_sample *));
}

void csbridge_tilde_setup(void)
{
    csbridge_class = class_new(gensym("csbridge~"), (t_newmethod)csbridge_new, (t_method)csbridge_destroy,
                               sizeof(t_csbridge), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(csbridge_class, t_csbridge, x_f);
    class_addmethod(csbridge_class, (t_method)csbridge_dsp,   gensym("dsp"),   A_CANT, 0);
    class_addmethod(csbridge_class, (t_method)csbridge_read,  gensym("read"),  A_SYMBOL, 0);
    class_addmethod(csbridge_class, (t_method)csbridge_reset, gensym("reset"), 0);
    class_addmethod(csbridge_class, (t_method)csbridge_chn,   gensym("chn"),   A_SYMBOL, A_FLOAT, 0);
    class_addmethod(csbridge_class, (t_method)csbridge_event, gensym("event"), A_GIMME, 0);
}
