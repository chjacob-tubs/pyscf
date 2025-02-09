#!/usr/bin/env python
#
# Author: Qiming Sun <osirpt.sun@gmail.com>
#

'''Non-relativistic DFT gradients'''

import time
import numpy
import scipy.linalg
from pyscf.lib import logger
from pyscf.scf import _vhf
import pyscf.dft
from pyscf.dft import numint
import pyscf.grad.hf


def get_veff_(ks, mol, dm):
    '''Coulomb + XC functional
    '''
    t0 = (time.clock(), time.time())
    assert(dm.ndim == 2)
    nao = dm.shape[0]

    if ks.grids.coords is None:
        ks.grids.build_()
    grids = ks.grids
    if ks._numint.non0tab is None:
        ks._numint.non0tab = ks._numint.make_mask(mol, ks.grids.coords)
    x_code, c_code = pyscf.dft.vxc.parse_xc_name(ks.xc)

    vxc = _get_vxc(ks._numint, mol, ks.grids, x_code, c_code, dm,
                   max_memory=mol.max_memory, verbose=ks.verbose)
    t0 = logger.timer(ks, 'vxc', *t0)

    hyb = pyscf.dft.vxc.hybrid_coeff(x_code, spin=(mol.spin>0)+1)

    if abs(hyb) < 1e-10:
        vj = _vhf.direct_mapdm('cint2e_ip1_sph',  # (nabla i,j|k,l)
                               's2kl', # ip1_sph has k>=l,
                               ('lk->s1ij',),
                               dm, 3, # xyz, 3 components
                               mol._atm, mol._bas, mol._env)
        vhf = vj
    else:
        vj, vk = _vhf.direct_mapdm('cint2e_ip1_sph',  # (nabla i,j|k,l)
                                   's2kl', # ip1_sph has k>=l,
                                   ('lk->s1ij', 'jk->s1il'),
                                   dm, 3, # xyz, 3 components
                                   mol._atm, mol._bas, mol._env)
        vhf = vj - vk * (hyb * .5)

    return -(vhf + vxc)


def _get_vxc(ni, mol, grids, x_id, c_id, dms, relativity=0, hermi=1,
             max_memory=2000, verbose=None):
    natocc = []
    natorb = []
    if isinstance(dms, numpy.ndarray) and dms.ndim == 2:
        e, c = scipy.linalg.eigh(dms)
        natocc.append(e)
        natorb.append(c)
        nao = dms.shape[0]
    else:
        for dm in dms:
            e, c = scipy.linalg.eigh(dm)
            natocc.append(e)
            natorb.append(c)
        nao = dms[0].shape[0]

    xctype = numint._xc_type(x_id, c_id)
    ngrids = len(grids.weights)
    BLKSIZE = numint.BLKSIZE
    blksize = min(int(max_memory/6*1e6/8/nao/BLKSIZE)*BLKSIZE, ngrids)

    nset = len(natocc)
    nelec = numpy.zeros(nset)
    excsum = numpy.zeros(nset)
    vmat = numpy.zeros((nset,3,nao,nao))
    if xctype == 'LDA':
        buf = numpy.empty((4,blksize,nao))
        for ip0, ip1 in numint.prange(0, ngrids, blksize):
            coords = grids.coords[ip0:ip1]
            weight = grids.weights[ip0:ip1]
            non0tab = ni.non0tab[ip0//BLKSIZE:]
            ao = ni.eval_ao(mol, coords, deriv=1, non0tab=non0tab, out=buf)
            for idm in range(nset):
                rho = ni.eval_rho2(mol, ao[0], natorb[idm], natocc[idm], non0tab, xctype)
                exc, vxc = ni.eval_xc(x_id, c_id, rho, 0, relativity, 1, verbose)[:2]
                vrho = vxc[0]
                den = rho * weight
                nelec[idm] += den.sum()
                excsum[idm] += (den * exc).sum()
                aow = numpy.einsum('pi,p->pi', ao[0], weight*vrho)
                vmat[idm,0] += numint._dot_ao_ao(mol, ao[1], aow, nao, ip1-ip0, non0tab)
                vmat[idm,1] += numint._dot_ao_ao(mol, ao[2], aow, nao, ip1-ip0, non0tab)
                vmat[idm,2] += numint._dot_ao_ao(mol, ao[3], aow, nao, ip1-ip0, non0tab)
                rho = exc = vxc = vrho = aow = None
    elif xctype == 'GGA':
        buf = numpy.empty((10,blksize,nao))
        XX, XY, XZ = 4, 5, 6
        YX, YY, YZ = 5, 7, 8
        ZX, ZY, ZZ = 6, 8, 9
        for ip0, ip1 in numint.prange(0, ngrids, blksize):
            coords = grids.coords[ip0:ip1]
            weight = grids.weights[ip0:ip1]
            non0tab = ni.non0tab[ip0//BLKSIZE:]
            ao = ni.eval_ao(mol, coords, deriv=2, non0tab=non0tab, out=buf)
            for idm in range(nset):
                rho = ni.eval_rho2(mol, ao, natorb[idm], natocc[idm], non0tab, xctype)
                exc, vxc = ni.eval_xc(x_id, c_id, rho, 0, relativity, 1, verbose)[:2]
                vrho, vsigma = vxc[:2]
                den = rho[0] * weight
                nelec[idm] += den.sum()
                excsum[idm] += (den * exc).sum()
                wv = numpy.empty_like(rho)
                # *.5 because vmat + vmat.T implicitly
                wv[0]  = weight * vrho * .5
                wv[1:] = rho[1:] * (weight * vsigma * 2)

                aow = numpy.einsum('npi,np->pi', ao[:4], wv)
                vmat[idm,0] += numint._dot_ao_ao(mol, ao[1], aow, nao, ip1-ip0, non0tab)
                vmat[idm,1] += numint._dot_ao_ao(mol, ao[2], aow, nao, ip1-ip0, non0tab)
                vmat[idm,2] += numint._dot_ao_ao(mol, ao[3], aow, nao, ip1-ip0, non0tab)

                aow = numpy.einsum('pi,p->pi', ao[1] , wv[0])
                aow+= numpy.einsum('pi,p->pi', ao[XX], wv[1])
                aow+= numpy.einsum('pi,p->pi', ao[XY], wv[2])
                aow+= numpy.einsum('pi,p->pi', ao[XZ], wv[3])
                vmat[idm,0] += numint._dot_ao_ao(mol, aow, ao[0], nao, ip1-ip0, non0tab)
                aow = numpy.einsum('pi,p->pi', ao[2] , wv[0])
                aow+= numpy.einsum('pi,p->pi', ao[YX], wv[1])
                aow+= numpy.einsum('pi,p->pi', ao[YY], wv[2])
                aow+= numpy.einsum('pi,p->pi', ao[YZ], wv[3])
                vmat[idm,1] += numint._dot_ao_ao(mol, aow, ao[0], nao, ip1-ip0, non0tab)
                aow = numpy.einsum('pi,p->pi', ao[3] , wv[0])
                aow+= numpy.einsum('pi,p->pi', ao[ZX], wv[1])
                aow+= numpy.einsum('pi,p->pi', ao[ZY], wv[2])
                aow+= numpy.einsum('pi,p->pi', ao[ZZ], wv[3])
                vmat[idm,2] += numint._dot_ao_ao(mol, aow, ao[0], nao, ip1-ip0, non0tab)
                rho = exc = vxc = vrho = vsigma = wv = aow = None
    else:
        raise NotImplementedError('meta-GGA')

    if nset == 1:
        vmat = vmat.reshape(3,nao,nao)
    return vmat


class RKS(pyscf.grad.hf.RHF):
    def __init__(self, scf_method):
        pyscf.grad.hf.RHF.__init__(self, scf_method)

    def get_veff(self, mol=None, dm=None):
        if mol is None: mol = self.mol
        if dm is None: dm = self._scf.make_rdm1()
        return get_veff_(self._scf, mol, dm)


if __name__ == '__main__':
    from pyscf import gto
    from pyscf import dft

    h2o = gto.Mole()
    h2o.verbose = 0
    h2o.output = None#'out_h2o'
    h2o.atom = [
        ['O' , (0. , 0.     , 0)],
        [1   , (0. , -0.757 , 0.587)],
        [1   , (0. ,  0.757 , 0.587)] ]
    h2o.basis = {'H': '631g',
                 'O': '631g',}
    h2o.build()
    mf = dft.RKS(h2o)
    mf.conv_tol = 1e-15
    print mf.scf()
    g = RKS(mf)
    print(g.grad())
#[[  1.37487273e-15  -1.80817689e-15   2.10512437e-02]
# [  4.62450121e-17   2.82055102e-02  -1.05251807e-02]
# [ -4.95856104e-17  -2.82055102e-02  -1.05251807e-02]]

    #mf.grids.level = 6
    mf.xc = 'b88,p86'
    print mf.scf()
    g = RKS(mf)
    print(g.grad())
#[[ -6.53044528e-16   1.61440998e-15   2.44607362e-02]
# [  2.99909644e-16   2.73756804e-02  -1.22322688e-02]
# [ -2.24487619e-16  -2.73756804e-02  -1.22322688e-02]]

    mf.xc = 'b3lyp'
    print mf.scf()
    g = RKS(mf)
    print(g.grad())
#[[ -3.44790653e-16  -2.31083509e-15   1.21670343e-02]
# [  7.15579513e-17   2.11176116e-02  -6.08866586e-03]
# [ -6.40735965e-17  -2.11176116e-02  -6.08866586e-03]]
