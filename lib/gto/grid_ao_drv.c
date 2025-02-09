/*
 * Author: Qiming Sun <osirpt.sun@gmail.com>
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "config.h"
#include "cint.h"
#include "vhf/fblas.h"

// 2 slots of int param[]
#define POS_E1   0
#define TENSOR   1

// 128s42p21d12f8g6h4i3j 
#define NCTR_CART      128
//  72s24p14d10f8g6h5i4j 
#define NCTR_SPH        72
#define NPRIMAX         64
#define BLKSIZE         96
#define EXPCUTOFF       50  // 1e-22
#define MIN(X,Y)        ((X)<(Y)?(X):(Y))
#define MAX(X,Y)        ((X)>(Y)?(X):(Y))
#define NOTZERO(e)      ((e)>1e-18 || (e)<-1e-18)

double exp_cephes(double x);
double CINTcommon_fac_sp(int l);

static int _len_cart[] = {
        1, 3, 6, 10, 15, 21, 28, 36
};

void GTOnabla1(double *fx1, double *fy1, double *fz1,
               double *fx0, double *fy0, double *fz0, int l, double a)
{
        int i;
        double a2 = -2 * a;
        fx1[0] = a2*fx0[1];
        fy1[0] = a2*fy0[1];
        fz1[0] = a2*fz0[1];
        for (i = 1; i <= l; i++) {
                fx1[i] = i*fx0[i-1] + a2*fx0[i+1];
                fy1[i] = i*fy0[i-1] + a2*fy0[i+1];
                fz1[i] = i*fz0[i-1] + a2*fz0[i+1];
        }
}

/*
 * r - R_O = (r-R_i) + ri, ri = (x,y,z) = R_i - R_O
 */
void GTOx1(double *fx1, double *fy1, double *fz1,
           double *fx0, double *fy0, double *fz0, int l,
           double x, double y, double z)
{
        int i;
        for (i = 0; i <= l; i++) {
                fx1[i] = x*fx0[i] + fx0[i+1];
                fy1[i] = y*fy0[i] + fy0[i+1];
                fz1[i] = z*fz0[i] + fz0[i+1];
        }
}

int GTOprim_exp(double *eprim, double *coord, double *alpha, double *coeff,
                int l, int nprim, int nctr, int blksize, double fac)
{
        int i, j;
        double arr, maxc;
        double logcoeff[nprim];
        double rr[blksize];
        double *gridx = coord;
        double *gridy = coord+blksize;
        double *gridz = coord+blksize*2;
        int not0 = 0;

        // the maximum value of the coefficients for each pGTO
        for (j = 0; j < nprim; j++) {
                maxc = 0;
                for (i = 0; i < nctr; i++) {
                        maxc = MAX(maxc, fabs(coeff[i*nprim+j]));
                }
                logcoeff[j] = log(maxc);
        }

        for (i = 0; i < blksize; i++) {
                rr[i] = gridx[i]*gridx[i] + gridy[i]*gridy[i] + gridz[i]*gridz[i];
        }

        for (j = 0; j < nprim; j++) {
                for (i = 0; i < blksize; i++) {
                        arr = alpha[j] * rr[i];
                        if (arr-logcoeff[j] < EXPCUTOFF) {
                                eprim[j*blksize+i] = exp_cephes(-arr) * fac;
                                not0 = 1;
                        } else {
                                eprim[j*blksize+i] = 0;
                        }
                }
        }
        return not0;
}


// grid2atm[atm_id,xyz,grid_id]
static void _fill_grid2atm(double *grid2atm, double *coord, int blksize,
                           int *atm, int natm, int *bas, int nbas, double *env)
{
        int atm_id, ig;
        double *r_atm;
        for (atm_id = 0; atm_id < natm; atm_id++) {
                r_atm = env + atm[PTR_COORD+atm_id*ATM_SLOTS];
                for (ig = 0; ig < blksize; ig++) {
                        grid2atm[0*blksize+ig] = coord[ig*3+0] - r_atm[0];
                        grid2atm[1*blksize+ig] = coord[ig*3+1] - r_atm[1];
                        grid2atm[2*blksize+ig] = coord[ig*3+2] - r_atm[2];
                }
                grid2atm += 3*blksize;
        }
}


static void _trans(double *ao, double *aobuf, int nao, int blksize, int counts)
{
        int i, j, k;
        if (blksize == BLKSIZE) {
                for (k = 0; k < BLKSIZE; k+=16) {
                        for (i = 0; i < counts; i++) {
                                for (j = k; j < k+16; j++) {
                                        ao[j*nao+i] = aobuf[i*BLKSIZE+j];
                                }
                        }
                }
        } else if ((blksize % 16) == 0) {
                for (k = 0; k < blksize; k+=16) {
                        for (i = 0; i < counts; i++) {
                                for (j = k; j < k+16; j++) {
                                        ao[j*nao+i] = aobuf[i*blksize+j];
                                }
                        }
                }
        } else {
                for (i = 0; i < counts; i++) {
                        for (j = 0; j < blksize; j++) {
                                ao[j*nao+i] = aobuf[j];
                        }
                        aobuf += blksize;
                }
        }
}

static void _set0(double *ao, int nao, int blksize, int counts)
{
        int i, j;
        for (j = 0; j < blksize; j++) {
                for (i = 0; i < counts; i++) {
                        ao[j*nao+i] = 0;
                }
        }
}

void GTOeval_sph_iter(void (*feval)(),  int (*fexp)(),
                      int param[], int nao, int ngrids,
                      int blksize, int bastart, int bascount,
                      double *ao, double *coord, char *non0table,
                      int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ncomp = param[TENSOR];
        const int basend = bastart + bascount;
        const int atmstart = bas[bastart*BAS_SLOTS+ATOM_OF];
        const int atmend = bas[(basend-1)*BAS_SLOTS+ATOM_OF]+1;
        const int atmcount = atmend - atmstart;
        int i, k, l, np, nc, atm_id, bas_id, deg;
        int ao_id = 0;
        double fac;
        double *p_exp, *pcoeff, *pcoord, *pcart, *ri;
        double *paobuf;
        double eprim[NPRIMAX*blksize*2];
        double cart_gto[NCTR_CART*blksize * ncomp];
        double aobuf[NCTR_SPH*blksize * ncomp];
        double grid2atm[atmcount*3*blksize]; // [atm_id,xyz,grid]

        _fill_grid2atm(grid2atm, coord, blksize,
                       atm+atmstart*ATM_SLOTS, atmcount, bas, nbas, env);

        for (bas_id = bastart; bas_id < basend; bas_id++) {
                np = bas[bas_id*BAS_SLOTS+NPRIM_OF];
                nc = bas[bas_id*BAS_SLOTS+NCTR_OF ];
                l  = bas[bas_id*BAS_SLOTS+ANG_OF  ];
                deg = l * 2 + 1;
                fac = CINTcommon_fac_sp(l);
                p_exp  = env + bas[bas_id*BAS_SLOTS+PTR_EXP];
                pcoeff = env + bas[bas_id*BAS_SLOTS+PTR_COEFF];
                atm_id = bas[bas_id*BAS_SLOTS+ATOM_OF];
                pcoord = grid2atm + (atm_id - atmstart) * 3*blksize;
                if (non0table[bas_id] &&
                    (*fexp)(eprim, pcoord, p_exp, pcoeff,
                            l, np, nc, blksize, fac)) {
                        ri = env + atm[PTR_COORD+atm_id*ATM_SLOTS];
                        (*feval)(cart_gto, ri, eprim, pcoord, p_exp, pcoeff,
                                 l, np, nc, blksize);
                        for (i = 0; i < ncomp; i++) {
                                pcart = cart_gto + i*nc*_len_cart[l]*blksize;
                                if (l < 2) { // s, p functions
                                        _trans(ao+i*nao*ngrids+ao_id, pcart,
                                               nao, blksize, nc*deg);
                                } else {
                                        paobuf = aobuf;
                                        for (k = 0; k < nc; k++) {
                                                CINTc2s_ket_sph(paobuf, blksize,
                                                                pcart, l);
                                                pcart += _len_cart[l] * blksize;
                                                paobuf += deg * blksize;
                                        }
                                        _trans(ao+i*nao*ngrids+ao_id, aobuf,
                                               nao, blksize, nc*deg);
                                }
                        }
                } else {
                        for (i = 0; i < ncomp; i++) {
                                _set0(ao+i*nao*ngrids+ao_id, nao, blksize, nc*deg);
                        }
                }
                ao_id += deg * nc;
        }
}

void GTOeval_cart_iter(void (*feval)(),  int (*fexp)(),
                       int param[], int nao, int ngrids,
                       int blksize, int bastart, int bascount,
                       double *ao, double *coord, char *non0table,
                       int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ncomp = param[TENSOR];
        const int basend = bastart + bascount;
        const int atmstart = bas[bastart*BAS_SLOTS+ATOM_OF];
        const int atmend = bas[(basend-1)*BAS_SLOTS+ATOM_OF]+1;
        const int atmcount = atmend - atmstart;
        int i, l, np, nc, atm_id, bas_id, deg;
        int ao_id = 0;
        double fac;
        double *p_exp, *pcoeff, *pcoord, *pcart, *ri;
        double eprim[NPRIMAX*blksize*2];
        double cart_gto[NCTR_CART*blksize * ncomp];
        double grid2atm[atmcount*3*blksize]; // [atm_id,xyz,grid]

        _fill_grid2atm(grid2atm, coord, blksize,
                       atm+atmstart*ATM_SLOTS, atmcount, bas, nbas, env);

        for (bas_id = bastart; bas_id < basend; bas_id++) {
                np = bas[bas_id*BAS_SLOTS+NPRIM_OF];
                nc = bas[bas_id*BAS_SLOTS+NCTR_OF ];
                l  = bas[bas_id*BAS_SLOTS+ANG_OF  ];
                deg = _len_cart[l];
                fac = CINTcommon_fac_sp(l);
                p_exp  = env + bas[bas_id*BAS_SLOTS+PTR_EXP];
                pcoeff = env + bas[bas_id*BAS_SLOTS+PTR_COEFF];
                atm_id = bas[bas_id*BAS_SLOTS+ATOM_OF];
                pcoord = grid2atm + (atm_id - atmstart) * 3*blksize;
                if (non0table[bas_id] &&
                    (*fexp)(eprim, pcoord, p_exp, pcoeff,
                            l, np, nc, blksize, fac)) {
                        ri = env + atm[PTR_COORD+atm_id*ATM_SLOTS];
                        (*feval)(cart_gto, ri, eprim, pcoord, p_exp, pcoeff,
                                 l, np, nc, blksize);
                        for (i = 0; i < ncomp; i++) {
                                pcart = cart_gto + i*nc*_len_cart[l]*blksize;
                                _trans(ao+i*nao*ngrids+ao_id, pcart,
                                       nao, blksize, nc*deg);
                        }
                } else {
                        for (i = 0; i < ncomp; i++) {
                                _set0(ao+i*nao*ngrids+ao_id, nao, blksize, nc*deg);
                        }
                }
                ao_id += deg * nc;
        }
}

/*
 * blksize <= 1024 to avoid stack overflow
 *
 * non0table[ngrids/blksize,natm] is the T/F table for ao values to
 * screen the ao evaluation for each shell
 */
void GTOeval_sph_drv(void (*feval)(), int (*fexp)(),
                     int param[], int nao, int ngrids,
                     int blksize, int bastart, int bascount,
                     double *ao, double *coord, char *non0table,
                     int *atm, int natm, int *bas, int nbas, double *env)
{
        ao += CINTtot_cgto_spheric(bas, bastart);

        const int nblk = (ngrids+blksize-1) / blksize;

        int ip, ib;
#pragma omp parallel default(none) \
        shared(feval, fexp, param, nao, ngrids, bastart, bascount, blksize, \
               ao, coord, non0table, atm, natm, bas, nbas, env) \
        private(ip, ib)
{
#pragma omp for nowait schedule(dynamic, 1)
        for (ib = 0; ib < nblk; ib++) {
                ip = ib * blksize;
                GTOeval_sph_iter(feval, fexp, param,
                                 nao, ngrids, MIN(ngrids-ip, blksize),
                                 bastart, bascount, ao+ip*nao, coord+ip*3,
                                 non0table+ib*nbas,
                                 atm, natm, bas, nbas, env);
        }
}
}

void GTOeval_cart_drv(void (*feval)(), int (*fexp)(),
                      int param[], int nao, int ngrids,
                      int blksize, int bastart, int bascount,
                      double *ao, double *coord, char *non0table,
                      int *atm, int natm, int *bas, int nbas, double *env)
{
        ao += CINTtot_cgto_cart(bas, bastart);

        const int nblk = (ngrids+blksize-1) / blksize;

        int ip, ib;
#pragma omp parallel default(none) \
        shared(feval, fexp, param, nao, ngrids, bastart, bascount, blksize, \
               ao, coord, non0table, atm, natm, bas, nbas, env) \
        private(ip, ib)
{
#pragma omp for nowait schedule(dynamic, 1)
        for (ib = 0; ib < nblk; ib++) {
                ip = ib * blksize;
                GTOeval_cart_iter(feval, fexp, param,
                                  nao, ngrids, MIN(ngrids-ip, blksize),
                                  bastart, bascount, ao+ip*nao, coord+ip*3,
                                  non0table+ib*nbas,
                                  atm, natm, bas, nbas, env);
        }
}
}

