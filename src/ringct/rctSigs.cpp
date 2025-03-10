// Copyright (c) 2016, Monero Research Labs
//
// Author: Shen Noether <shen.noether@gmx.com>
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "misc_log_ex.h"
#include "misc_language.h"
#include "common/perf_timer.h"
#include "common/threadpool.h"
#include "common/util.h"
#include "rctSigs.h"
#include "bulletproofs.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_config.h"
#include <boost/multiprecision/cpp_int.hpp>
#include "offshore/asset_types.h"

using namespace crypto;
using namespace std;

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "ringct"

#define CHECK_AND_ASSERT_MES_L1(expr, ret, message) {if(!(expr)) {MCERROR("verify", message); return ret;}}

namespace
{
  rct::Bulletproof make_dummy_bulletproof(const std::vector<uint64_t> &outamounts, rct::keyV &C, rct::keyV &masks)
  {
    const size_t n_outs = outamounts.size();
    const rct::key I = rct::identity();
    size_t nrl = 0;
    while ((1u << nrl) < n_outs)
      ++nrl;
    nrl += 6;

    C.resize(n_outs);
    masks.resize(n_outs);
    for (size_t i = 0; i < n_outs; ++i)
    {
      masks[i] = I;
      rct::key sv8, sv;
      sv = rct::zero();
      sv.bytes[0] = outamounts[i] & 255;
      sv.bytes[1] = (outamounts[i] >> 8) & 255;
      sv.bytes[2] = (outamounts[i] >> 16) & 255;
      sv.bytes[3] = (outamounts[i] >> 24) & 255;
      sv.bytes[4] = (outamounts[i] >> 32) & 255;
      sv.bytes[5] = (outamounts[i] >> 40) & 255;
      sv.bytes[6] = (outamounts[i] >> 48) & 255;
      sv.bytes[7] = (outamounts[i] >> 56) & 255;
      sc_mul(sv8.bytes, sv.bytes, rct::INV_EIGHT.bytes);
      rct::addKeys2(C[i], rct::INV_EIGHT, sv8, rct::H);
    }

    return rct::Bulletproof{rct::keyV(n_outs, I), I, I, I, I, I, I, rct::keyV(nrl, I), rct::keyV(nrl, I), I, I, I};
  }
  
  rct::key sm(rct::key y, int n, const rct::key &x)
  {
    while (n--)
      sc_mul(y.bytes, y.bytes, y.bytes);
    sc_mul(y.bytes, y.bytes, x.bytes);
    return y;
  }

  /* Compute the inverse of a scalar, the clever way */
  rct::key invert(const rct::key &x)
  {
    rct::key _1, _10, _100, _11, _101, _111, _1001, _1011, _1111;

    _1 = x;
    sc_mul(_10.bytes, _1.bytes, _1.bytes);
    sc_mul(_100.bytes, _10.bytes, _10.bytes);
    sc_mul(_11.bytes, _10.bytes, _1.bytes);
    sc_mul(_101.bytes, _10.bytes, _11.bytes);
    sc_mul(_111.bytes, _10.bytes, _101.bytes);
    sc_mul(_1001.bytes, _10.bytes, _111.bytes);
    sc_mul(_1011.bytes, _10.bytes, _1001.bytes);
    sc_mul(_1111.bytes, _100.bytes, _1011.bytes);

    rct::key inv;
    sc_mul(inv.bytes, _1111.bytes, _1.bytes);

    inv = sm(inv, 123 + 3, _101);
    inv = sm(inv, 2 + 2, _11);
    inv = sm(inv, 1 + 4, _1111);
    inv = sm(inv, 1 + 4, _1111);
    inv = sm(inv, 4, _1001);
    inv = sm(inv, 2, _11);
    inv = sm(inv, 1 + 4, _1111);
    inv = sm(inv, 1 + 3, _101);
    inv = sm(inv, 3 + 3, _101);
    inv = sm(inv, 3, _111);
    inv = sm(inv, 1 + 4, _1111);
    inv = sm(inv, 2 + 3, _111);
    inv = sm(inv, 2 + 2, _11);
    inv = sm(inv, 1 + 4, _1011);
    inv = sm(inv, 2 + 4, _1011);
    inv = sm(inv, 6 + 4, _1001);
    inv = sm(inv, 2 + 2, _11);
    inv = sm(inv, 3 + 2, _11);
    inv = sm(inv, 3 + 2, _11);
    inv = sm(inv, 1 + 4, _1001);
    inv = sm(inv, 1 + 3, _111);
    inv = sm(inv, 2 + 4, _1111);
    inv = sm(inv, 1 + 4, _1011);
    inv = sm(inv, 3, _101);
    inv = sm(inv, 2 + 4, _1111);
    inv = sm(inv, 3, _101);
    inv = sm(inv, 1 + 2, _11);

    // Sanity check for successful inversion
    rct::key tmp;
    sc_mul(tmp.bytes, inv.bytes, x.bytes);
    CHECK_AND_ASSERT_THROW_MES(tmp == rct::identity(), "invert failed");
    return inv;
  }
  
}

namespace rct {

  Bulletproof proveRangeBulletproof(keyV &C, keyV &masks, const std::vector<uint64_t> &amounts, epee::span<const key> sk, hw::device &hwdev)
  {
    CHECK_AND_ASSERT_THROW_MES(amounts.size() == sk.size(), "Invalid amounts/sk sizes");
    masks.resize(amounts.size());
    for (size_t i = 0; i < masks.size(); ++i)
        masks[i] = hwdev.genCommitmentMask(sk[i]);
    Bulletproof proof = bulletproof_PROVE(amounts, masks);
    CHECK_AND_ASSERT_THROW_MES(proof.V.size() == amounts.size(), "V does not have the expected size");
    C = proof.V;
    return proof;
  }

  bool verBulletproof(const Bulletproof &proof)
  {
    try { return bulletproof_VERIFY(proof); }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (...) { return false; }
  }

  bool verBulletproof(const std::vector<const Bulletproof*> &proofs)
  {
    try { return bulletproof_VERIFY(proofs); }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (...) { return false; }
  }
  
  //see above.
  bool verifyBorromean(const boroSig &bb, const ge_p3 P1[64], const ge_p3 P2[64]) {
    key64 Lv1; key chash, LL;
    int ii = 0;
    ge_p2 p2;
    for (ii = 0 ; ii < 64 ; ii++) {
        // equivalent of: addKeys2(LL, bb.s0[ii], bb.ee, P1[ii]);
        ge_double_scalarmult_base_vartime(&p2, bb.ee.bytes, &P1[ii], bb.s0[ii].bytes);
        ge_tobytes(LL.bytes, &p2);
        chash = hash_to_scalar(LL);
        // equivalent of: addKeys2(Lv1[ii], bb.s1[ii], chash, P2[ii]);
        ge_double_scalarmult_base_vartime(&p2, chash.bytes, &P2[ii], bb.s1[ii].bytes);
        ge_tobytes(Lv1[ii].bytes, &p2);
    }
    key eeComputed = hash_to_scalar(Lv1); //hash function fine
    return equalKeys(eeComputed, bb.ee);
  }

  bool verifyBorromean(const boroSig &bb, const key64 P1, const key64 P2) {
    ge_p3 P1_p3[64], P2_p3[64];
    for (size_t i = 0 ; i < 64 ; ++i) {
      CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&P1_p3[i], P1[i].bytes) == 0, false, "point conv failed");
      CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&P2_p3[i], P2[i].bytes) == 0, false, "point conv failed");
    }
    return verifyBorromean(bb, P1_p3, P2_p3);
  }
  // Generate a CLSAG signature
  // See paper by Goodell et al. (https://eprint.iacr.org/2019/654)
  //
  // The keys are set as follows:
  //   P[l] == p*G
  //   C[l] == z*G
  //   C[i] == C_nonzero[i] - C_offset (for hashing purposes) for all i
  clsag CLSAG_Gen(const key &message, const keyV & P, const key & p, const keyV & C, const key & z, const keyV & C_nonzero, const key & C_offset, const unsigned int l, const multisig_kLRki *kLRki, key *mscout, key *mspout) {
    clsag sig;
    size_t n = P.size(); // ring size
    CHECK_AND_ASSERT_THROW_MES(n == C.size(), "Signing and commitment key vector sizes must match!");
    CHECK_AND_ASSERT_THROW_MES(n == C_nonzero.size(), "Signing and commitment key vector sizes must match!");
    CHECK_AND_ASSERT_THROW_MES(l < n, "Signing index out of range!");
    CHECK_AND_ASSERT_THROW_MES((kLRki && mscout) || (!kLRki && !mscout), "Only one of kLRki/mscout is present");
    CHECK_AND_ASSERT_THROW_MES((mscout && mspout) || !kLRki, "Multisig pointers are not all present");

    // Key images
    ge_p3 H_p3;
    hash_to_p3(H_p3,P[l]);
    key H;
    ge_p3_tobytes(H.bytes,&H_p3);

    key D;
    scalarmultKey(D,H,z);

    // Multisig
    if (kLRki)
    {
        sig.I = kLRki->ki;
    }
    else
    {
        scalarmultKey(sig.I,H,p);
    }

    geDsmp I_precomp;
    geDsmp D_precomp;
    precomp(I_precomp.k,sig.I);
    precomp(D_precomp.k,D);

    // Offset key image
    scalarmultKey(sig.D,D,INV_EIGHT);

    // Initial values
    key a;
    key aG;
    key aH;
    skpkGen(a,aG);
    scalarmultKey(aH,H,a);

    // Aggregation hashes
    keyV mu_P_to_hash(2*n+4); // domain, I, D, P, C, C_offset
    keyV mu_C_to_hash(2*n+4); // domain, I, D, P, C, C_offset
    sc_0(mu_P_to_hash[0].bytes);
    memcpy(mu_P_to_hash[0].bytes,config::HASH_KEY_CLSAG_AGG_0,sizeof(config::HASH_KEY_CLSAG_AGG_0)-1);
    sc_0(mu_C_to_hash[0].bytes);
    memcpy(mu_C_to_hash[0].bytes,config::HASH_KEY_CLSAG_AGG_1,sizeof(config::HASH_KEY_CLSAG_AGG_1)-1);
    for (size_t i = 1; i < n+1; ++i) {
        mu_P_to_hash[i] = P[i-1];
        mu_C_to_hash[i] = P[i-1];
    }
    for (size_t i = n+1; i < 2*n+1; ++i) {
        mu_P_to_hash[i] = C_nonzero[i-n-1];
        mu_C_to_hash[i] = C_nonzero[i-n-1];
    }
    mu_P_to_hash[2*n+1] = sig.I;
    mu_P_to_hash[2*n+2] = sig.D;
    mu_P_to_hash[2*n+3] = C_offset;
    mu_C_to_hash[2*n+1] = sig.I;
    mu_C_to_hash[2*n+2] = sig.D;
    mu_C_to_hash[2*n+3] = C_offset;
    key mu_P, mu_C;
    mu_P = hash_to_scalar(mu_P_to_hash);
    mu_C = hash_to_scalar(mu_C_to_hash);

    // Initial commitment
    keyV c_to_hash(2*n+5); // domain, P, C, C_offset, message, aG, aH
    key c;
    sc_0(c_to_hash[0].bytes);
    memcpy(c_to_hash[0].bytes,config::HASH_KEY_CLSAG_ROUND,sizeof(config::HASH_KEY_CLSAG_ROUND)-1);
    for (size_t i = 1; i < n+1; ++i)
    {
        c_to_hash[i] = P[i-1];
        c_to_hash[i+n] = C_nonzero[i-1];
    }
    c_to_hash[2*n+1] = C_offset;
    c_to_hash[2*n+2] = message;

    // Multisig data is present
    if (kLRki)
    {
        a = kLRki->k;
        c_to_hash[2*n+3] = kLRki->L;
        c_to_hash[2*n+4] = kLRki->R;
    }
    else
    {
        c_to_hash[2*n+3] = aG;
        c_to_hash[2*n+4] = aH;
    }
    c = hash_to_scalar(c_to_hash);
    
    size_t i;
    i = (l + 1) % n;
    if (i == 0)
        copy(sig.c1, c);

    // Decoy indices
    sig.s = keyV(n);
    key c_new;
    key L;
    key R;
    key c_p; // = c[i]*mu_P
    key c_c; // = c[i]*mu_C
    geDsmp P_precomp;
    geDsmp C_precomp;
    geDsmp H_precomp;
    ge_p3 Hi_p3;

    while (i != l) {
        sig.s[i] = skGen();
        sc_0(c_new.bytes);
        sc_mul(c_p.bytes,mu_P.bytes,c.bytes);
        sc_mul(c_c.bytes,mu_C.bytes,c.bytes);

        // Precompute points
        precomp(P_precomp.k,P[i]);
        precomp(C_precomp.k,C[i]);

        // Compute L
        addKeys_aGbBcC(L,sig.s[i],c_p,P_precomp.k,c_c,C_precomp.k);

        // Compute R
        hash_to_p3(Hi_p3,P[i]);
        ge_dsm_precomp(H_precomp.k, &Hi_p3);
        addKeys_aAbBcC(R,sig.s[i],H_precomp.k,c_p,I_precomp.k,c_c,D_precomp.k);

        c_to_hash[2*n+3] = L;
        c_to_hash[2*n+4] = R;
        c_new = hash_to_scalar(c_to_hash);
        copy(c,c_new);
        
        i = (i + 1) % n;
        if (i == 0)
            copy(sig.c1,c);
    }

    // Compute final scalar
    key s0_p_mu_P;
    sc_mul(s0_p_mu_P.bytes,mu_P.bytes,p.bytes);
    key s0_add_z_mu_C;
    sc_muladd(s0_add_z_mu_C.bytes,mu_C.bytes,z.bytes,s0_p_mu_P.bytes);
    sc_mulsub(sig.s[l].bytes,c.bytes,s0_add_z_mu_C.bytes,a.bytes);

    if (mscout)
      *mscout = c;
    if (mspout)
      *mspout = mu_P;

    return sig;
  }

  clsag CLSAG_Gen(const key &message, const keyV & P, const key & p, const keyV & C, const key & z, const keyV & C_nonzero, const key & C_offset, const unsigned int l) {
    return CLSAG_Gen(message, P, p, C, z, C_nonzero, C_offset, l, NULL, NULL, NULL);
  }
    
  // MLSAG signatures
  // See paper by Noether (https://eprint.iacr.org/2015/1098)
  // This generalization allows for some dimensions not to require linkability;
  //   this is used in practice for commitment data within signatures
  // Note that using more than one linkable dimension is not recommended.
  bool MLSAG_Ver(const key &message, const keyM & pk, const mgSig & rv, size_t dsRows) {
    size_t cols = pk.size();
    CHECK_AND_ASSERT_MES(cols >= 2, false, "Signature must contain more than one public key");
    size_t rows = pk[0].size();
    CHECK_AND_ASSERT_MES(rows >= 1, false, "Bad total row number");
    for (size_t i = 1; i < cols; ++i) {
      CHECK_AND_ASSERT_MES(pk[i].size() == rows, false, "Bad public key matrix dimensions");
    }
    CHECK_AND_ASSERT_MES(rv.II.size() == dsRows, false, "Wrong number of key images present");
    CHECK_AND_ASSERT_MES(rv.ss.size() == cols, false, "Bad scalar matrix dimensions");
    for (size_t i = 0; i < cols; ++i) {
      CHECK_AND_ASSERT_MES(rv.ss[i].size() == rows, false, "Bad scalar matrix dimensions");
    }
    CHECK_AND_ASSERT_MES(dsRows <= rows, false, "Non-double-spend rows cannot exceed total rows");

    for (size_t i = 0; i < rv.ss.size(); ++i) {
      for (size_t j = 0; j < rv.ss[i].size(); ++j) {
        CHECK_AND_ASSERT_MES(sc_check(rv.ss[i][j].bytes) == 0, false, "Bad signature scalar");
      }
    }
    CHECK_AND_ASSERT_MES(sc_check(rv.cc.bytes) == 0, false, "Bad initial signature hash");

    size_t i = 0, j = 0, ii = 0;
    key c,  L, R;
    key c_old = copy(rv.cc);
    vector<geDsmp> Ip(dsRows);
    for (i = 0 ; i < dsRows ; i++) {
        CHECK_AND_ASSERT_MES(!(rv.II[i] == rct::identity()), false, "Bad key image");
        precomp(Ip[i].k, rv.II[i]);
    }
    size_t ndsRows = 3 * dsRows; // number of dimensions not requiring linkability
    keyV toHash(1 + 3 * dsRows + 2 * (rows - dsRows));
    toHash[0] = message;
    i = 0;
    while (i < cols) {
        sc_0(c.bytes);
        for (j = 0; j < dsRows; j++) {
            addKeys2(L, rv.ss[i][j], c_old, pk[i][j]);

            // Compute R directly
            ge_p3 hash8_p3;
            hash_to_p3(hash8_p3, pk[i][j]);
            ge_p2 R_p2;
            ge_double_scalarmult_precomp_vartime(&R_p2, rv.ss[i][j].bytes, &hash8_p3, c_old.bytes, Ip[j].k);
            ge_tobytes(R.bytes, &R_p2);

            toHash[3 * j + 1] = pk[i][j];
            toHash[3 * j + 2] = L; 
            toHash[3 * j + 3] = R;
        }
        for (j = dsRows, ii = 0 ; j < rows ; j++, ii++) {
            addKeys2(L, rv.ss[i][j], c_old, pk[i][j]);
            toHash[ndsRows + 2 * ii + 1] = pk[i][j];
            toHash[ndsRows + 2 * ii + 2] = L;
        }
        c = hash_to_scalar(toHash);
        CHECK_AND_ASSERT_MES(!(c == rct::zero()), false, "Bad signature hash");
        copy(c_old, c);
        i = (i + 1);
    }
    sc_sub(c.bytes, c_old.bytes, rv.cc.bytes);
    return sc_isnonzero(c.bytes) == 0;
  }
  
  //proveRange and verRange
  //proveRange gives C, and mask such that \sumCi = C
  //   c.f. https://eprint.iacr.org/2015/1098 section 5.1
  //   and Ci is a commitment to either 0 or 2^i, i=0,...,63
  //   thus this proves that "amount" is in [0, 2^64]
  //   mask is a such that C = aG + bH, and b = amount
  //verRange verifies that \sum Ci = C and that each Ci is a commitment to 0 or 2^i
  bool verRange(const key & C, const rangeSig & as) {
    try
    {
      PERF_TIMER(verRange);
      ge_p3 CiH[64], asCi[64];
      int i = 0;
      ge_p3 Ctmp_p3 = ge_p3_identity;
      for (i = 0; i < 64; i++) {
          // faster equivalent of:
          // subKeys(CiH[i], as.Ci[i], H2[i]);
          // addKeys(Ctmp, Ctmp, as.Ci[i]);
          ge_cached cached;
          ge_p3 p3;
          ge_p1p1 p1;
          CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&p3, H2[i].bytes) == 0, false, "point conv failed");
          ge_p3_to_cached(&cached, &p3);
          CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&asCi[i], as.Ci[i].bytes) == 0, false, "point conv failed");
          ge_sub(&p1, &asCi[i], &cached);
          ge_p3_to_cached(&cached, &asCi[i]);
          ge_p1p1_to_p3(&CiH[i], &p1);
          ge_add(&p1, &Ctmp_p3, &cached);
          ge_p1p1_to_p3(&Ctmp_p3, &p1);
      }
      key Ctmp;
      ge_p3_tobytes(Ctmp.bytes, &Ctmp_p3);
      if (!equalKeys(C, Ctmp))
        return false;
      if (!verifyBorromean(as.asig, asCi, CiH))
        return false;
      return true;
    }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (...) { return false; }
  }

  key get_pre_mlsag_hash(const rctSig &rv, hw::device &hwdev)
  {
    keyV hashes;
    hashes.reserve(3);
    hashes.push_back(rv.message);
    crypto::hash h;

    std::stringstream ss;
    binary_archive<true> ba(ss);
    CHECK_AND_ASSERT_THROW_MES(!rv.mixRing.empty(), "Empty mixRing");
    const size_t inputs = is_rct_simple(rv.type) ? rv.mixRing.size() : rv.mixRing[0].size();
    const size_t outputs = rv.ecdhInfo.size();
    key prehash;
    CHECK_AND_ASSERT_THROW_MES(const_cast<rctSig&>(rv).serialize_rctsig_base(ba, inputs, outputs),
        "Failed to serialize rctSigBase");
    cryptonote::get_blob_hash(ss.str(), h);
    hashes.push_back(hash2rct(h));

    keyV kv;
    if (rv.type == RCTTypeBulletproof || rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3)
    {
      kv.reserve((6*2+9) * rv.p.bulletproofs.size());
      for (const auto &p: rv.p.bulletproofs)
      {
        // V are not hashed as they're expanded from outPk.mask
        // (and thus hashed as part of rctSigBase above)
        kv.push_back(p.A);
        kv.push_back(p.S);
        kv.push_back(p.T1);
        kv.push_back(p.T2);
        kv.push_back(p.taux);
        kv.push_back(p.mu);
        for (size_t n = 0; n < p.L.size(); ++n)
          kv.push_back(p.L[n]);
        for (size_t n = 0; n < p.R.size(); ++n)
          kv.push_back(p.R[n]);
        kv.push_back(p.a);
        kv.push_back(p.b);
        kv.push_back(p.t);
      }
    }
    else
    {
      kv.reserve((64*3+1) * rv.p.rangeSigs.size());
      for (const auto &r: rv.p.rangeSigs)
      {
        for (size_t n = 0; n < 64; ++n)
          kv.push_back(r.asig.s0[n]);
        for (size_t n = 0; n < 64; ++n)
          kv.push_back(r.asig.s1[n]);
        kv.push_back(r.asig.ee);
        for (size_t n = 0; n < 64; ++n)
          kv.push_back(r.Ci[n]);
      }
    }
    hashes.push_back(cn_fast_hash(kv));
    hwdev.mlsag_prehash(ss.str(), inputs, outputs, hashes, rv.outPk, prehash);
    return  prehash;
  }

  clsag proveRctCLSAGSimple(const key &message, const ctkeyV &pubs, const ctkey &inSk, const key &a, const key &Cout, const multisig_kLRki *kLRki, key *mscout, key *mspout, unsigned int index, hw::device &hwdev) {
    //setup vars
    size_t rows = 1;
    size_t cols = pubs.size();
    CHECK_AND_ASSERT_THROW_MES(cols >= 1, "Empty pubs");
    CHECK_AND_ASSERT_THROW_MES((kLRki && mscout) || (!kLRki && !mscout), "Only one of kLRki/mscout is present");
    keyV tmp(rows + 1);
    keyV sk(rows + 1);
    size_t i;
    keyM M(cols, tmp);

    keyV P, C, C_nonzero;
    P.reserve(pubs.size());
    C.reserve(pubs.size());
    C_nonzero.reserve(pubs.size());
    for (const ctkey &k: pubs)
    {
        P.push_back(k.dest);
        C_nonzero.push_back(k.mask);
        rct::key tmp;
        subKeys(tmp, k.mask, Cout);
        C.push_back(tmp);
    }

    sk[0] = copy(inSk.dest);
    sc_sub(sk[1].bytes, inSk.mask.bytes, a.bytes);
    clsag result = CLSAG_Gen(message, P, sk[0], C, sk[1], C_nonzero, Cout, index, kLRki, mscout, mspout);
    memwipe(&sk[0], sizeof(key));
    return result;
  }


  //Ring-ct MG sigs
  //Prove: 
  //   c.f. https://eprint.iacr.org/2015/1098 section 4. definition 10. 
  //   This does the MG sig on the "dest" part of the given key matrix, and 
  //   the last row is the sum of input commitments from that column - sum output commitments
  //   this shows that sum inputs = sum outputs
  //Ver:    
  //   verifies the above sig is created corretly
  bool verRctMG(const mgSig &mg, const ctkeyM & pubs, const ctkeyV & outPk, const key &txnFeeKey, const key &message) {
    PERF_TIMER(verRctMG);
    //setup vars
    size_t cols = pubs.size();
    CHECK_AND_ASSERT_MES(cols >= 1, false, "Empty pubs");
    size_t rows = pubs[0].size();
    CHECK_AND_ASSERT_MES(rows >= 1, false, "Empty pubs");
    for (size_t i = 1; i < cols; ++i) {
      CHECK_AND_ASSERT_MES(pubs[i].size() == rows, false, "pubs is not rectangular");
    }

    keyV tmp(rows + 1);
    size_t i = 0, j = 0;
    for (i = 0; i < rows + 1; i++) {
        identity(tmp[i]);
    }
    keyM M(cols, tmp);

    //create the matrix to mg sig
    for (j = 0; j < rows; j++) {
        for (i = 0; i < cols; i++) {
            M[i][j] = pubs[i][j].dest;
            addKeys(M[i][rows], M[i][rows], pubs[i][j].mask); //add Ci in last row
        }
    }
    for (i = 0; i < cols; i++) {
        for (j = 0; j < outPk.size(); j++) {
            subKeys(M[i][rows], M[i][rows], outPk[j].mask); //subtract output Ci's in last row
        }
        //subtract txn fee output in last row
        subKeys(M[i][rows], M[i][rows], txnFeeKey);
    }
    return MLSAG_Ver(message, M, mg, rows);
  }

  //Ring-ct Simple MG sigs
  //Ver: 
  //This does a simplified version, assuming only post Rct
  //inputs
  bool verRctMGSimple(const key &message, const mgSig &mg, const ctkeyV & pubs, const key & C) {
    try
    {
      PERF_TIMER(verRctMGSimple);
      //setup vars
      size_t rows = 1;
      size_t cols = pubs.size();
      CHECK_AND_ASSERT_MES(cols >= 1, false, "Empty pubs");
      keyV tmp(rows + 1);
      size_t i;
      keyM M(cols, tmp);
      ge_p3 Cp3;
      CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&Cp3, C.bytes) == 0, false, "point conv failed");
      ge_cached Ccached;
      ge_p3_to_cached(&Ccached, &Cp3);
      ge_p1p1 p1;
      //create the matrix to mg sig
      for (i = 0; i < cols; i++) {
              M[i][0] = pubs[i].dest;
              ge_p3 p3;
              CHECK_AND_ASSERT_MES_L1(ge_frombytes_vartime(&p3, pubs[i].mask.bytes) == 0, false, "point conv failed");
              ge_sub(&p1, &p3, &Ccached);
              ge_p1p1_to_p3(&p3, &p1);
              ge_p3_tobytes(M[i][1].bytes, &p3);
      }
      //DP(C);
      return MLSAG_Ver(message, M, mg, rows);
    }
    catch (...) { return false; }
  }

  bool verRctCLSAGSimple(const key &message, const clsag &sig, const ctkeyV & pubs, const key & C_offset) {
    try
    {
      PERF_TIMER(verRctCLSAGSimple);
      const size_t n = pubs.size();

      // Check data
      CHECK_AND_ASSERT_MES(n >= 1, false, "Empty pubs");
      CHECK_AND_ASSERT_MES(n == sig.s.size(), false, "Signature scalar vector is the wrong size!");
      for (size_t i = 0; i < n; ++i)
          CHECK_AND_ASSERT_MES(sc_check(sig.s[i].bytes) == 0, false, "Bad signature scalar!");
      CHECK_AND_ASSERT_MES(sc_check(sig.c1.bytes) == 0, false, "Bad signature commitment!");
      CHECK_AND_ASSERT_MES(!(sig.I == rct::identity()), false, "Bad key image!");

      // Cache commitment offset for efficient subtraction later
      ge_p3 C_offset_p3;
      CHECK_AND_ASSERT_MES(ge_frombytes_vartime(&C_offset_p3, C_offset.bytes) == 0, false, "point conv failed");
      ge_cached C_offset_cached;
      ge_p3_to_cached(&C_offset_cached, &C_offset_p3);

      // Prepare key images
      key c = copy(sig.c1);
      key D_8 = scalarmult8(sig.D);
      CHECK_AND_ASSERT_MES(!(D_8 == rct::identity()), false, "Bad auxiliary key image!");
      geDsmp I_precomp;
      geDsmp D_precomp;
      precomp(I_precomp.k,sig.I);
      precomp(D_precomp.k,D_8);

      // Aggregation hashes
      keyV mu_P_to_hash(2*n+4); // domain, I, D, P, C, C_offset
      keyV mu_C_to_hash(2*n+4); // domain, I, D, P, C, C_offset
      sc_0(mu_P_to_hash[0].bytes);
      memcpy(mu_P_to_hash[0].bytes,config::HASH_KEY_CLSAG_AGG_0,sizeof(config::HASH_KEY_CLSAG_AGG_0)-1);
      sc_0(mu_C_to_hash[0].bytes);
      memcpy(mu_C_to_hash[0].bytes,config::HASH_KEY_CLSAG_AGG_1,sizeof(config::HASH_KEY_CLSAG_AGG_1)-1);
      for (size_t i = 1; i < n+1; ++i) {
          mu_P_to_hash[i] = pubs[i-1].dest;
          mu_C_to_hash[i] = pubs[i-1].dest;
      }
      for (size_t i = n+1; i < 2*n+1; ++i) {
          mu_P_to_hash[i] = pubs[i-n-1].mask;
          mu_C_to_hash[i] = pubs[i-n-1].mask;
      }
      mu_P_to_hash[2*n+1] = sig.I;
      mu_P_to_hash[2*n+2] = sig.D;
      mu_P_to_hash[2*n+3] = C_offset;
      mu_C_to_hash[2*n+1] = sig.I;
      mu_C_to_hash[2*n+2] = sig.D;
      mu_C_to_hash[2*n+3] = C_offset;
      key mu_P, mu_C;
      mu_P = hash_to_scalar(mu_P_to_hash);
      mu_C = hash_to_scalar(mu_C_to_hash);

      // Set up round hash
      keyV c_to_hash(2*n+5); // domain, P, C, C_offset, message, L, R
      sc_0(c_to_hash[0].bytes);
      memcpy(c_to_hash[0].bytes,config::HASH_KEY_CLSAG_ROUND,sizeof(config::HASH_KEY_CLSAG_ROUND)-1);
      for (size_t i = 1; i < n+1; ++i)
      {
          c_to_hash[i] = pubs[i-1].dest;
          c_to_hash[i+n] = pubs[i-1].mask;
      }
      c_to_hash[2*n+1] = C_offset;
      c_to_hash[2*n+2] = message;
      key c_p; // = c[i]*mu_P
      key c_c; // = c[i]*mu_C
      key c_new;
      key L;
      key R;
      geDsmp P_precomp;
      geDsmp C_precomp;
      geDsmp H_precomp;
      size_t i = 0;
      ge_p3 hash8_p3;
      geDsmp hash_precomp;
      ge_p3 temp_p3;
      ge_p1p1 temp_p1;

      while (i < n) {
        sc_0(c_new.bytes);
        sc_mul(c_p.bytes,mu_P.bytes,c.bytes);
        sc_mul(c_c.bytes,mu_C.bytes,c.bytes);

        // Precompute points for L/R
        precomp(P_precomp.k,pubs[i].dest);

        CHECK_AND_ASSERT_MES(ge_frombytes_vartime(&temp_p3, pubs[i].mask.bytes) == 0, false, "point conv failed");
        ge_sub(&temp_p1,&temp_p3,&C_offset_cached);
        ge_p1p1_to_p3(&temp_p3,&temp_p1);
        ge_dsm_precomp(C_precomp.k,&temp_p3);

        // Compute L
        addKeys_aGbBcC(L,sig.s[i],c_p,P_precomp.k,c_c,C_precomp.k);

        // Compute R
        hash_to_p3(hash8_p3,pubs[i].dest);
        ge_dsm_precomp(hash_precomp.k, &hash8_p3);
        addKeys_aAbBcC(R,sig.s[i],hash_precomp.k,c_p,I_precomp.k,c_c,D_precomp.k);

        c_to_hash[2*n+3] = L;
        c_to_hash[2*n+4] = R;
        c_new = hash_to_scalar(c_to_hash);
        CHECK_AND_ASSERT_MES(!(c_new == rct::zero()), false, "Bad signature hash");
        copy(c,c_new);

        i = i + 1;
      }
      sc_sub(c_new.bytes,c.bytes,sig.c1.bytes);
      return sc_isnonzero(c_new.bytes) == 0;
    }
    catch (...) { return false; }
  }

  //These functions get keys from blockchain
  //replace these when connecting blockchain
  //getKeyFromBlockchain grabs a key from the blockchain at "reference_index" to mix with
  //populateFromBlockchain creates a keymatrix with "mixin" columns and one of the columns is inPk
  //   the return value are the key matrix, and the index where inPk was put (random).    
  void getKeyFromBlockchain(ctkey & a, size_t reference_index) {
    a.mask = pkGen();
    a.dest = pkGen();
  }

  //These functions get keys from blockchain
  //replace these when connecting blockchain
  //getKeyFromBlockchain grabs a key from the blockchain at "reference_index" to mix with
  //populateFromBlockchain creates a keymatrix with "mixin" + 1 columns and one of the columns is inPk
  //   the return value are the key matrix, and the index where inPk was put (random).     
  tuple<ctkeyM, xmr_amount> populateFromBlockchain(ctkeyV inPk, int mixin) {
    int rows = inPk.size();
    ctkeyM rv(mixin + 1, inPk);
    int index = randXmrAmount(mixin);
    int i = 0, j = 0;
    for (i = 0; i <= mixin; i++) {
        if (i != index) {
            for (j = 0; j < rows; j++) {
                getKeyFromBlockchain(rv[i][j], (size_t)randXmrAmount);
            }
        }
    }
    return make_tuple(rv, index);
  }

  //These functions get keys from blockchain
  //replace these when connecting blockchain
  //getKeyFromBlockchain grabs a key from the blockchain at "reference_index" to mix with
  //populateFromBlockchain creates a keymatrix with "mixin" columns and one of the columns is inPk
  //   the return value are the key matrix, and the index where inPk was put (random).     
  xmr_amount populateFromBlockchainSimple(ctkeyV & mixRing, const ctkey & inPk, int mixin) {
    int index = randXmrAmount(mixin);
    int i = 0;
    for (i = 0; i <= mixin; i++) {
        if (i != index) {
            getKeyFromBlockchain(mixRing[i], (size_t)randXmrAmount(1000));
        } else {
            mixRing[i] = inPk;
        }
    }
    return index;
  }
    
  //RCT simple    
  //for post-rct only
  rctSig genRctSimple(
    const key & message, 
    const ctkeyV & inSk, 
    const keyV & destinations, 
    const std::vector<xmr_amount> & inamounts,
    const std::vector<size_t>& inamounts_col_indices,
    const uint64_t onshore_col_amount,
    const std::string in_asset_type, 
    const std::vector<std::pair<std::string,std::pair<xmr_amount,bool>>> & outamounts, 
    xmr_amount txnFee,
    xmr_amount txnOffshoreFee,
    const ctkeyM & mixRing, 
    const keyV &amount_keys, 
    const std::vector<multisig_kLRki> *kLRki, 
    multisig_out *msout, 
    const std::vector<unsigned int> & index, 
    ctkeyV &outSk, 
    const RCTConfig &rct_config, 
    hw::device &hwdev, 
    const offshore::pricing_record& pr,
    uint8_t tx_version
  ){

    // Sanity checks
    CHECK_AND_ASSERT_THROW_MES(inamounts.size() > 0, "Empty inamounts");
    CHECK_AND_ASSERT_THROW_MES(inamounts.size() == inSk.size(), "Different number of inamounts/inSk");
    CHECK_AND_ASSERT_THROW_MES(outamounts.size() == destinations.size(), "Different number of amounts/destinations");
    CHECK_AND_ASSERT_THROW_MES(amount_keys.size() == destinations.size(), "Different number of amount_keys/destinations");
    CHECK_AND_ASSERT_THROW_MES(index.size() == inSk.size(), "Different number of index/inSk");
    CHECK_AND_ASSERT_THROW_MES(mixRing.size() == inSk.size(), "Different number of mixRing/inSk");
    for (size_t n = 0; n < mixRing.size(); ++n) {
      CHECK_AND_ASSERT_THROW_MES(index[n] < mixRing[n].size(), "Bad index into mixRing");
    }
    CHECK_AND_ASSERT_THROW_MES((kLRki && msout) || (!kLRki && !msout), "Only one of kLRki/msout is present");
    if (kLRki && msout) {
      CHECK_AND_ASSERT_THROW_MES(kLRki->size() == inamounts.size(), "Mismatched kLRki/inamounts sizes");
    }

    // These are different outamounts structures for differen purposes.
    // outamounts_rangesig is only used for barmanian prove(pre-bulletproof) and for only for regular XHV Txs.
    // outamounts_flat_amounts only contains the output amounts without asset_type
    std::vector<xmr_amount> outamounts_rangesig, outamounts_flat_amounts;
    
    // Work out the type of the TX from the mix of inputs and outputs
    bool xhv_sent = false, usd_sent = false, xasset_sent = false; 
    if (in_asset_type == "XHV") {
      xhv_sent = true;
    } else if (in_asset_type == "XUSD") {
      usd_sent = true;
    } else {
      xasset_sent = true;
    }

    // NEAC: convert outgoing amount vector/pairs into discrete vectors
    bool xhv_received = false, usd_received = false, xasset_received = false; 
    for (size_t i = 0 ; i < outamounts.size(); i++) {
      if (outamounts[i].first == "XHV") {
        outamounts_rangesig.push_back(outamounts[i].second.first);
        xhv_received = true;
      } else if (outamounts[i].first == "XUSD") {
        outamounts_rangesig.push_back(0);
        usd_received = true;
      } else {
        outamounts_rangesig.push_back(0);
        xasset_received = true;
      }
      // The second value is always non-zero - just copy it in all conditions so we can build bulletproof vector
      outamounts_flat_amounts.push_back(outamounts[i].second.first);
    }

    // Set the transaction type.
    rctSig rv;
    switch (rct_config.bp_version)
    {
      case 0:
      case 6:
        rv.type = RCTTypeHaven3;
        break;
      case 5:
        rv.type = RCTTypeHaven2;
        break;
      case 4:
        rv.type = RCTTypeCLSAGN;
        break;
      case 3:
        rv.type = RCTTypeCLSAG;
        break;
      case 2:
        rv.type = RCTTypeBulletproof2;
        break;
      case 1:
        rv.type = RCTTypeBulletproof;
        break;
      default:
        ASSERT_MES_AND_THROW("Unsupported BP version: " << rct_config.bp_version);
    }
    
    // Determine the tx direction
    bool offshore = (xhv_sent && !usd_sent && usd_received && xhv_received);
    bool onshore = (usd_sent && !xhv_sent && usd_received && xhv_received);
    bool offshore_to_offshore = (usd_sent && !xhv_sent && usd_received && !xhv_received);
    bool xasset_to_xusd = (xasset_sent && xasset_received && usd_received);
    bool xusd_to_xasset = (usd_sent && xasset_received && usd_received);
    bool xasset_transfer = (xasset_sent && xasset_received && !usd_received);
    bool conversion_tx = (offshore || onshore || xusd_to_xasset || xasset_to_xusd);
    bool use_onshore_col = (onshore && rv.type == RCTTypeHaven3);

    // prepare the rct data structures
    rv.message = message;
    rv.outPk.resize(destinations.size());
    rv.outPk_usd.resize(destinations.size());
    rv.outPk_xasset.resize(destinations.size());
    rv.ecdhInfo.resize(destinations.size());

    // prove range pre-bulletproof
    size_t i;
    keyV masks(destinations.size()); //sk mask..
    outSk.resize(destinations.size());
    for (i = 0; i < destinations.size(); i++) {
      //add destination to sig
      rv.outPk[i].dest = rv.outPk_usd[i].dest = rv.outPk_xasset[i].dest = copy(destinations[i]);
    }

    // do the bulletproofs
    key zerokey = rct::identity();
    rv.p.bulletproofs.clear();
    if (rv.type == RCTTypeHaven3 && conversion_tx) {
      rv.maskSums.resize(3);
      rv.maskSums[0] = zero();
      rv.maskSums[1] = zero();
      rv.maskSums[2] = zero();
    } else if (rv.type == RCTTypeHaven2) {
      rv.maskSums.resize(2);
      rv.maskSums[0] = zero();
      rv.maskSums[1] = zero();
    }
    size_t n_amounts = outamounts.size();
    size_t amounts_proved = 0;
    if (rct_config.range_proof_type == RangeProofPaddedBulletproof)
    {
      rct::keyV C, D, masks_xhv, masks_usd;
      if (hwdev.get_mode() == hw::device::TRANSACTION_CREATE_FAKE)
      {
        // use a fake bulletproof for speed
        rv.p.bulletproofs.push_back(make_dummy_bulletproof(outamounts_flat_amounts, C, masks));
      }
      else
      {
        const epee::span<const key> keys{&amount_keys[0], amount_keys.size()};
        rv.p.bulletproofs.push_back(proveRangeBulletproof(C, masks, outamounts_flat_amounts, keys, hwdev));
        #ifdef DBG
        CHECK_AND_ASSERT_THROW_MES(verBulletproof(rv.p.bulletproofs.back()), "verBulletproof failed on newly created proof");
        #endif
      }

      for (i = 0; i < outamounts.size(); ++i)
      {
        if (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3) {
          rv.outPk[i].mask = rct::scalarmult8(C[i]);
          if (outamounts[i].first == "XHV" && offshore) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          } else if ((outamounts[i].first == "XUSD") &&
                      (onshore || xusd_to_xasset)) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          } else if ((outamounts[i].first != "XUSD") &&
                      (xasset_to_xusd)) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          }

          if (rv.type == RCTTypeHaven3) {
            // save the col output mask for offshore
            if (offshore && outamounts[i].second.second) {
              sc_add(rv.maskSums[2].bytes, rv.maskSums[2].bytes, masks[i].bytes);
            }
            
            // save the actual col output(not change) mask for onshore
            if (use_onshore_col && outamounts[i].second.second && outamounts[i].second.first == onshore_col_amount) {
              rv.maskSums[2] = masks[i];
            }
          }
        } else {
          if (outamounts[i].first == "XHV") {
            rv.outPk[i].mask = rct::scalarmult8(C[i]);
            rv.outPk_usd[i].mask = zerokey;
            rv.outPk_xasset[i].mask = zerokey;
          } else if (outamounts[i].first == "XUSD") {
            rv.outPk[i].mask = zerokey;
            rv.outPk_usd[i].mask = rct::scalarmult8(C[i]);
            rv.outPk_xasset[i].mask = zerokey;
          } else {
            rv.outPk[i].mask = zerokey;
            rv.outPk_usd[i].mask = zerokey;
            rv.outPk_xasset[i].mask = rct::scalarmult8(C[i]);
          }
        }
        outSk[i].mask = masks[i];
      }
    }
    else while (amounts_proved < n_amounts)
    {
      size_t batch_size = 1;
      if (rct_config.range_proof_type == RangeProofMultiOutputBulletproof)
        while (batch_size * 2 + amounts_proved <= n_amounts && batch_size * 2 <= BULLETPROOF_MAX_OUTPUTS)
          batch_size *= 2;

      rct::keyV C, masks;
      std::vector<uint64_t> batch_amounts(batch_size);
      for (i = 0; i < batch_size; ++i) {
        batch_amounts[i] = outamounts_flat_amounts[i + amounts_proved];
      }
      if (hwdev.get_mode() == hw::device::TRANSACTION_CREATE_FAKE)
      {
        // use a fake bulletproof for speed
        rv.p.bulletproofs.push_back(make_dummy_bulletproof(batch_amounts, C, masks));
      }
      else
      {
        const epee::span<const key> keys{&amount_keys[amounts_proved], batch_size};
        rv.p.bulletproofs.push_back(proveRangeBulletproof(C, masks, batch_amounts, keys, hwdev));
        #ifdef DBG
        CHECK_AND_ASSERT_THROW_MES(verBulletproof(rv.p.bulletproofs.back()), "verBulletproof failed on newly created proof");
        #endif
      }
      for (i = 0; i < batch_size; ++i)
      {
        if (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3) {
          rv.outPk[i + amounts_proved].mask = rct::scalarmult8(C[i]);
          if ((outamounts[i + amounts_proved].first == "XHV") && offshore) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          } else if ((outamounts[i + amounts_proved].first == "XUSD") && (onshore || xusd_to_xasset)) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          } else if ((outamounts[i + amounts_proved].first != "XUSD") &&
                      (xasset_to_xusd)) {
            // we know these are change outputs
            sc_add(rv.maskSums[1].bytes, rv.maskSums[1].bytes, masks[i].bytes);
          }
        } else {
          if (outamounts[i + amounts_proved].first == "XHV") {
            rv.outPk[i + amounts_proved].mask = rct::scalarmult8(C[i]);
            rv.outPk_usd[i + amounts_proved].mask = zerokey;
            rv.outPk_xasset[i + amounts_proved].mask = zerokey;
          } else if (outamounts[i + amounts_proved].first == "XUSD") {
            rv.outPk[i + amounts_proved].mask = zerokey;
            rv.outPk_usd[i + amounts_proved].mask = rct::scalarmult8(C[i]);
            rv.outPk_xasset[i + amounts_proved].mask = zerokey;
          } else {
            rv.outPk[i + amounts_proved].mask = zerokey;
            rv.outPk_usd[i + amounts_proved].mask = zerokey;
            rv.outPk_xasset[i + amounts_proved].mask = rct::scalarmult8(C[i]);
          }
        }
        outSk[i + amounts_proved].mask = masks[i];
      }
      amounts_proved += batch_size;
    }

    // do the output encryption and asset conversions
    key sumout = zero();
    key sumout_onshore_col = zero();
    key atomic = d2h(COIN);
    key inverse_atomic = invert(atomic);
    for (i = 0; i < outSk.size(); ++i)
    {
      key outSk_scaled = zero();
      key tempkey = zero();
      if (in_asset_type == "XHV") {
        // SPENDING XHV
        if (outamounts[i].first == "XUSD") {
          // OFFSHORE - Convert output amount to XHV for equalKeys() testing
          key inverse_rate = invert(d2h((tx_version >= POU_TRANSACTION_VERSION ? std::min(pr.unused1, pr.xUSD) : pr.unused1)));
          sc_mul(tempkey.bytes, outSk[i].mask.bytes, atomic.bytes);
          sc_mul(outSk_scaled.bytes, tempkey.bytes, inverse_rate.bytes);
        } else {
          // Output amount in XHV already - no conversion required
          outSk_scaled = outSk[i].mask;
        }
      } else if (in_asset_type == "XUSD") {
        // SPENDING XUSD
        if (outamounts[i].first == "XUSD") {
          // Output amount in USD already - no conversion required
          outSk_scaled = outSk[i].mask;
        } else if (outamounts[i].first == "XHV" && !outamounts[i].second.second) { 
          // ONSHORE - convert output amount to USD for equalKeys() testing
          key rate = d2h(tx_version >= POU_TRANSACTION_VERSION ? std::max(pr.unused1, pr.xUSD) : pr.unused1);
          sc_mul(tempkey.bytes, outSk[i].mask.bytes, rate.bytes);
          sc_mul(outSk_scaled.bytes, tempkey.bytes, inverse_atomic.bytes);
        } else if (outamounts[i].first != "XHV" &&  outamounts[i].first != "XUSD") {
          // xAsset equivalent to OFFSHORE - convert output amount to USD for equalKeys() testing
          key inverse_rate_xasset = invert(d2h(pr[outamounts[i].first]));
          sc_mul(tempkey.bytes, outSk[i].mask.bytes, atomic.bytes);
          sc_mul(outSk_scaled.bytes, tempkey.bytes, inverse_rate_xasset.bytes);
        } else {
          // onshore col output
          outSk_scaled = outSk[i].mask;
        }
      } else {
        // SPENDING XASSET
        if (outamounts[i].first == "XUSD") {
          // xAsset equivalent to ONSHORE - convert output amount to USD for equalKeys() testing
          key rate_xasset = d2h(pr[in_asset_type]);
          sc_mul(tempkey.bytes, outSk[i].mask.bytes, rate_xasset.bytes);
          sc_mul(outSk_scaled.bytes, tempkey.bytes, inverse_atomic.bytes);
        } else if (outamounts[i].first == "XHV") {
          // SHOULD NOT BE POSSIBLE!!!
        } else {
          // Output amount in xAsset already - no conversion required
          outSk_scaled = outSk[i].mask;
        }
      }

      // exclude the onshore collateral outs(actual + change)
      if (use_onshore_col && outamounts[i].second.second) {
        sc_add(sumout_onshore_col.bytes, outSk_scaled.bytes, sumout_onshore_col.bytes);
      } else {
        sc_add(sumout.bytes, outSk_scaled.bytes, sumout.bytes);
      }
      
      //mask amount and mask
      rv.ecdhInfo[i].mask = copy(outSk[i].mask);
      rv.ecdhInfo[i].amount = d2h(outamounts_flat_amounts[i]);
      hwdev.ecdhEncode(rv.ecdhInfo[i], amount_keys[i], rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3);
    }
            
    //set txn fee
    if (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3) {
      rv.txnFee = txnFee;
      rv.txnOffshoreFee = txnOffshoreFee;
    } else {
      if  (in_asset_type == "XHV") {
        rv.txnFee = txnFee;
        rv.txnOffshoreFee = txnOffshoreFee;
      } else if (in_asset_type == "XUSD") {
        rv.txnFee_usd = txnFee;
        rv.txnOffshoreFee_usd = txnOffshoreFee;
      } else {
        rv.txnFee_xasset = txnFee;
        rv.txnOffshoreFee_xasset = txnOffshoreFee;
      }
    }

    // set the ring and pseudoOuts
    rv.mixRing = mixRing;
    keyV &pseudoOuts = rv.p.pseudoOuts;
    pseudoOuts.resize(inamounts.size());

    // prepare CLSAGs or MLSAGs vectors
    if ((rv.type == RCTTypeCLSAG) || (rv.type == RCTTypeCLSAGN) || (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3))
      rv.p.CLSAGs.resize(inamounts.size());
    else
      rv.p.MGs.resize(inamounts.size());

    
    // separete the actual and colleteral inputs
    std::vector<std::pair<size_t, uint64_t>> actual_in_amounts;
    std::vector<std::pair<size_t, uint64_t>> onshore_col_in_amounts;
    for (i = 0; i < inamounts.size(); i++) {
      auto it = std::find(inamounts_col_indices.begin(), inamounts_col_indices.end(), i);
      if (it != inamounts_col_indices.end())
        onshore_col_in_amounts.push_back(std::pair<size_t, uint64_t>(i, inamounts[i]));
      else
        actual_in_amounts.push_back(std::pair<size_t, uint64_t>(i, inamounts[i]));
    }

    // generate commitments per input
    key sumpouts = zero(); //sum pseudoOut masks
    keyV a(inamounts.size());
    for (i = 0 ; i < actual_in_amounts.size() - 1; i++) {
      // Generate a random key
      skGen(a[actual_in_amounts[i].first]);
      // Sum the random keys as we iterate
      sc_add(sumpouts.bytes, a[actual_in_amounts[i].first].bytes, sumpouts.bytes);
      // Generate a commitment to the amount with the random key
      genC(pseudoOuts[actual_in_amounts[i].first], a[actual_in_amounts[i].first], actual_in_amounts[i].second);
    }
    sc_sub(a[actual_in_amounts[i].first].bytes, sumout.bytes, sumpouts.bytes);
    genC(pseudoOuts[actual_in_amounts[i].first], a[actual_in_amounts[i].first], actual_in_amounts[i].second);

    // set the sum of input blinding factors
    if (conversion_tx && (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3)) {
      sc_add(rv.maskSums[0].bytes, a[actual_in_amounts[i].first].bytes, sumpouts.bytes);
    }

    // generate the commitments for collateral inputs
    if (use_onshore_col) {
      sumpouts = zero();
      for (i = 0; i < onshore_col_in_amounts.size() - 1; i++) {
        // Generate a random key
        skGen(a[onshore_col_in_amounts[i].first]);
        // Sum the random keys as we iterate
        sc_add(sumpouts.bytes, a[onshore_col_in_amounts[i].first].bytes, sumpouts.bytes);
        // Generate a commitment to the amount with the random key
        genC(pseudoOuts[onshore_col_in_amounts[i].first], a[onshore_col_in_amounts[i].first], onshore_col_in_amounts[i].second);
      }
      sc_sub(a[onshore_col_in_amounts[i].first].bytes, sumout_onshore_col.bytes, sumpouts.bytes);
      genC(pseudoOuts[onshore_col_in_amounts[i].first], a[onshore_col_in_amounts[i].first], onshore_col_in_amounts[i].second);
    }
    DP(pseudoOuts[onshore_col_in_amounts[i].first]);

    key full_message = get_pre_mlsag_hash(rv,hwdev);
    if (msout)
    {
      msout->c.resize(inamounts.size());
      msout->mu_p.resize((rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3) ? inamounts.size() : 0);
    }

    // do a CLSAG signing for each input
    for (i = 0 ; i < inamounts.size(); i++)
    {
      rv.p.CLSAGs[i] = proveRctCLSAGSimple(full_message, rv.mixRing[i], inSk[i], a[i], pseudoOuts[i], kLRki ? &(*kLRki)[i]: NULL, msout ? &msout->c[i] : NULL, msout ? &msout->mu_p[i] : NULL, index[i], hwdev);
    }

    return rv;
  }

  rctSig genRctSimple(
    const key & message, 
    const ctkeyV & inSk, 
    const ctkeyV & inPk, 
    const keyV & destinations, 
    const std::vector<xmr_amount> & inamounts,
    const std::vector<size_t>& inamounts_col_indices,
    const uint64_t onshore_col_amount,
    const std::string in_asset_type, 
    const std::vector<std::pair<std::string,std::pair<xmr_amount,bool>>> & outamounts, 
    const keyV &amount_keys, 
    const std::vector<multisig_kLRki> *kLRki, 
    multisig_out *msout, 
    xmr_amount txnFee,
    xmr_amount txnOffshoreFee,
    unsigned int mixin, 
    const RCTConfig &rct_config, 
    hw::device &hwdev, 
    const offshore::pricing_record& pr,
    uint8_t tx_version
  ){
    std::vector<unsigned int> index;
    index.resize(inPk.size());
    ctkeyM mixRing;
    ctkeyV outSk;
    mixRing.resize(inPk.size());
    for (size_t i = 0; i < inPk.size(); ++i) {
      mixRing[i].resize(mixin+1);
      index[i] = populateFromBlockchainSimple(mixRing[i], inPk[i], mixin);
    }
    return  genRctSimple(
      message,
      inSk,
      destinations,
      inamounts,
      inamounts_col_indices,
      onshore_col_amount,
      in_asset_type,
      outamounts,
      txnFee,
      txnOffshoreFee,
      mixRing,
      amount_keys,
      kLRki,
      msout,
      index,
      outSk,
      rct_config,
      hwdev,
      pr,
      tx_version
    );
  }

  //RingCT protocol
  //genRct: 
  //   creates an rctSig with all data necessary to verify the rangeProofs and that the signer owns one of the
  //   columns that are claimed as inputs, and that the sum of inputs  = sum of outputs.
  //   Also contains masked "amount" and "mask" so the receiver can see how much they received
  //verRct:
  //   verifies that all signatures (rangeProogs, MG sig, sum inputs = outputs) are correct
  //decodeRct: (c.f. https://eprint.iacr.org/2015/1098 section 5.1.1)
  //   uses the attached ecdh info to find the amounts represented by each output commitment 
  //   must know the destination private key to find the correct amount, else will return a random number    
  bool verRct(const rctSig & rv, bool semantics) {
    PERF_TIMER(verRct);
    CHECK_AND_ASSERT_MES(rv.type == RCTTypeFull, false, "verRct called on non-full rctSig");
    if (semantics)
    {
      CHECK_AND_ASSERT_MES(rv.outPk.size() == rv.p.rangeSigs.size(), false, "Mismatched sizes of outPk and rv.p.rangeSigs");
      CHECK_AND_ASSERT_MES(rv.outPk.size() == rv.ecdhInfo.size(), false, "Mismatched sizes of outPk and rv.ecdhInfo");
      CHECK_AND_ASSERT_MES(rv.p.MGs.size() == 1, false, "full rctSig has not one MG");
    }
    else
    {
      // semantics check is early, we don't have the MGs resolved yet
    }

    // some rct ops can throw
    try
    {
      if (semantics) {
        tools::threadpool& tpool = tools::threadpool::getInstance();
        tools::threadpool::waiter waiter;
        std::deque<bool> results(rv.outPk.size(), false);
        DP("range proofs verified?");
        for (size_t i = 0; i < rv.outPk.size(); i++)
          tpool.submit(&waiter, [&, i] { results[i] = verRange(rv.outPk[i].mask, rv.p.rangeSigs[i]); });
        waiter.wait(&tpool);

        for (size_t i = 0; i < results.size(); ++i) {
          if (!results[i]) {
            LOG_PRINT_L1("Range proof verified failed for proof " << i);
            return false;
          }
        }
      }

      if (!semantics) {
        //compute txn fee
        key txnFeeKey = scalarmultH(d2h(rv.txnFee));
        bool mgVerd = verRctMG(rv.p.MGs[0], rv.mixRing, rv.outPk, txnFeeKey, get_pre_mlsag_hash(rv, hw::get_device("default")));
        DP("mg sig verified?");
        DP(mgVerd);
        if (!mgVerd) {
          LOG_PRINT_L1("MG signature verification failed");
          return false;
        }
      }

      return true;
    }
    catch (const std::exception &e)
    {
      LOG_PRINT_L1("Error in verRct: " << e.what());
      return false;
    }
    catch (...)
    {
      LOG_PRINT_L1("Error in verRct, but not an actual exception");
      return false;
    }
  }

  // yC = constant for USD/XHV exchange rate
  // Ci = pseudoOuts[i] *** Ci & Di are MUTUALLY EXCLUSIVE
  // fcG' = fee in XHV = 0
  // C'k = outPk[k].mask
  // yD = constant for XHV/USD exchange rate (1/yC)
  // Di = pseudoOuts[i] *** Ci & Di are MUTUALLY EXCLUSIVE
  // fdG' = fee in USD = 0
  // D'k = outPk_usd[k].mask
  //
  //ver RingCT simple
  //assumes only post-rct style inputs (at least for max anonymity)
  bool verRctSemanticsSimple2(
    const rctSig& rv, 
    const offshore::pricing_record& pr,
    const cryptonote::transaction_type& tx_type,
    const std::string& strSource, 
    const std::string& strDest,
    uint64_t amount_burnt,
    const std::vector<cryptonote::tx_out> &vout,
    const std::vector<cryptonote::txin_v> &vin,
    const uint8_t version,
    const std::vector<uint32_t>& collateral_indices,
    const uint64_t amount_collateral
  ){

    try
    {
      PERF_TIMER(verRctSemanticsSimple2);

      tools::threadpool& tpool = tools::threadpool::getInstance();
      tools::threadpool::waiter waiter;
      std::deque<bool> results;
      std::vector<const Bulletproof*> proofs;
      size_t max_non_bp_proofs = 0, offset = 0;
      using tt = cryptonote::transaction_type;

      CHECK_AND_ASSERT_MES(rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3, false, "verRctSemanticsSimple2 called on non-Haven2 rctSig");

      const bool bulletproof = is_rct_bulletproof(rv.type);
      CHECK_AND_ASSERT_MES(bulletproof, false, "Only bulletproofs supported for Haven2");
      CHECK_AND_ASSERT_MES(rv.outPk.size() == n_bulletproof_amounts(rv.p.bulletproofs), false, "Mismatched sizes of outPk and bulletproofs");
      CHECK_AND_ASSERT_MES(rv.p.MGs.empty(), false, "MGs are not empty for CLSAG");
      CHECK_AND_ASSERT_MES(rv.p.pseudoOuts.size() == rv.p.CLSAGs.size(), false, "Mismatched sizes of rv.p.pseudoOuts and rv.p.CLSAGs");
      CHECK_AND_ASSERT_MES(rv.pseudoOuts.empty(), false, "rv.pseudoOuts is not empty");
      CHECK_AND_ASSERT_MES(rv.outPk.size() == rv.ecdhInfo.size(), false, "Mismatched sizes of outPk and rv.ecdhInfo");
      if (rv.type == RCTTypeHaven2) 
        CHECK_AND_ASSERT_MES(rv.maskSums.size() == 2, false, "maskSums size is not 2");
      CHECK_AND_ASSERT_MES(std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strSource) != offshore::ASSET_TYPES.end(), false, "Invalid Source Asset!");
      CHECK_AND_ASSERT_MES(std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strDest) != offshore::ASSET_TYPES.end(), false, "Invalid Dest Asset!");
      CHECK_AND_ASSERT_MES(tx_type != tt::UNSET, false, "Invalid transaction type.");
      if (strSource != strDest) {
        CHECK_AND_ASSERT_MES(!pr.empty(), false, "Empty pricing record found for a conversion tx");
        CHECK_AND_ASSERT_MES(amount_burnt, false, "0 amount_burnt found for a conversion tx");
        if (rv.type == RCTTypeHaven3) {
          CHECK_AND_ASSERT_MES(rv.maskSums.size() == 3, false, "maskSums size is not correct");
          CHECK_AND_ASSERT_MES(collateral_indices.size() == 2, false, "collateral indices size is not 2");
          if (tx_type == tt::OFFSHORE || tx_type == tt::ONSHORE)
            CHECK_AND_ASSERT_MES(amount_collateral, false, "0 collateral requirement something went wrong! rejecting tx..");
        }
      }
      
      // OUTPUTS SUMMED FOR EACH COLOUR
      key zerokey = rct::identity();
      // Zi is intentionally set to a different value to zerokey, so that if a bug is introduced in the later logic, any comparison with zerokey will fail
      key Zi = scalarmultH(d2h(1));

      // Calculate sum of all C' and D'
      rct::keyV masks_C;
      rct::keyV masks_D;
      size_t i = 0;
      for (auto &output: vout) {
        bool onshore_col_idx = false;
        if (version >= HF_VERSION_USE_COLLATERAL) {
          // make sure onshore check is always first, it is segfault otherwise since col_indices are empty for transfers
          if (tx_type == tt::ONSHORE && (i == collateral_indices[0] || i == collateral_indices[1]))
            onshore_col_idx = true;
        }
        std::string output_asset_type;
        if (output.target.type() == typeid(cryptonote::txout_to_key)) {
          output_asset_type = "XHV";
        } else if (output.target.type() == typeid(cryptonote::txout_offshore)) {
          output_asset_type = "XUSD";
        } else if (output.target.type() == typeid(cryptonote::txout_xasset)) {
          output_asset_type = boost::get<cryptonote::txout_xasset>(output.target).asset_type;
        } else {
          LOG_PRINT_L1("Invalid output type detected");
          return false;
        }

        // exclude the onshore collateral ouputs from proof-of-value calculation
        if (!onshore_col_idx) {
          if (output_asset_type == strSource) {
            masks_C.push_back(rv.outPk[i].mask);
          } else if (output_asset_type == strDest) {
            masks_D.push_back(rv.outPk[i].mask);
          } else {
            LOG_PRINT_L1("Invalid output detected (wrong asset type)");
            return false;
          }
        }
        i++;
      }
      key sumOutpks_C = addKeys(masks_C);
      key sumOutpks_D = addKeys(masks_D);
      DP(sumOutpks_C);
      DP(sumOutpks_D);

      // FEES FOR EACH COLOUR
      // Calculate tx fee for C colour
      key txnFeeKey = scalarmultH(d2h(rv.txnFee));
      // Calculate offshore conversion fee (also always in C colour)
      key txnOffshoreFeeKey = scalarmultH(d2h(rv.txnOffshoreFee));

      /*
        offshore TX:
        sumPseudoOuts = addKeys(pseudoOuts); (total of inputs)
        sumPseudoOuts_usd = zerokey; (no input usd amount)

        sumXHV = total_output_value_in_XHV (after subtracting fees)
        sumUSD = -total_output_value_in_USD

        D_scaled = sumUSD 
        yC_invert = 1 / exchange_rate_in_usd
        D_final = -total_output_value_in_XHV
        Zi = total_output_value_in_XHV - total_output_value_in_XHV = 0; 


        XUSD -> XASSET TX:
        sumPseudoOuts_usd = total_input_in_usd
        sumPseudoOuts_xasset = zerokey; (no input xasset amount)


        sumUSD = total_output_value_in_USD (after subtracting fees)
        sumXASSET = -total_output_value_in_XASSET (without fees)

        D_scaled = sumXASSET
        y = exchange_rate_in_usd
        D_final = sumXASSET * 1/ exchange_rate_in_usd = -total_output_value_in_USD
        Zi = sumUSD + D_final = 0
      */

      // exclude the onshore collateral inputs from proof-of-value calculation
      key sumPseudoOuts = zerokey;
      key sumColIns = zerokey;
      if (tx_type == tt::ONSHORE && version >= HF_VERSION_USE_COLLATERAL) {
        for (size_t i = 0; i < rv.p.pseudoOuts.size(); ++i) {
          if (vin[i].type() == typeid(cryptonote::txin_to_key)) {
            sumColIns = addKeys(sumColIns, rv.p.pseudoOuts[i]);
          } else {
            sumPseudoOuts = addKeys(sumPseudoOuts, rv.p.pseudoOuts[i]);
          }
        }
      } else {
        sumPseudoOuts = addKeys(rv.p.pseudoOuts);
      }
      DP(sumPseudoOuts);

      // C COLOUR
      key sumC;
      // Remove the fees
      subKeys(sumC, sumPseudoOuts, txnFeeKey);
      subKeys(sumC, sumC, txnOffshoreFeeKey);
      subKeys(sumC, sumC, sumOutpks_C);

      // D COLOUR
      key sumD;
      // Subtract the sum of converted output commitments from the sum of consumed output commitments in D colour (if any are present)
      // (Note: there are only consumed output commitments in D colour if the transaction is an onshore and requires collateral)
      subKeys(sumD, zerokey, sumOutpks_D);

      // NEAC: attempt to only calculate forward
      // CALCULATE Zi
      if (tx_type == tt::OFFSHORE) {
        key D_scaled = scalarmultKey(sumD, d2h(COIN));
        key yC_invert = invert(d2h((version >= HF_PER_OUTPUT_UNLOCK_VERSION) ? std::min(pr.unused1, pr.xUSD) : pr.unused1));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumC, D_final);
      } else if (tx_type == tt::ONSHORE) {
        key D_scaled = scalarmultKey(sumD, d2h((version >= HF_PER_OUTPUT_UNLOCK_VERSION) ? std::max(pr.unused1, pr.xUSD) : pr.unused1));
        key yC_invert = invert(d2h(COIN));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumC, D_final);
      } else if (tx_type == tt::OFFSHORE_TRANSFER) {
        Zi = addKeys(sumC, sumD);
      } else if (tx_type == tt::XUSD_TO_XASSET) {
        key D_scaled = scalarmultKey(sumD, d2h(COIN));
        key yC_invert = invert(d2h(pr[strDest]));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumC, D_final);
      } else if (tx_type == tt::XASSET_TO_XUSD) {
        key D_scaled = scalarmultKey(sumD, d2h(pr[strSource]));
        key yC_invert = invert(d2h(COIN));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumC, D_final);
      } else if (tx_type == tt::XASSET_TRANSFER) {
        Zi = addKeys(sumC, sumD);
      } else if (tx_type == tt::TRANSFER) {
        Zi = addKeys(sumC, sumD);
      } else {
        LOG_PRINT_L1("Invalid transaction type specified");
        return false;
      }

      //check Zi == 0
      if (!equalKeys(Zi, zerokey)) {
        LOG_PRINT_L1("Sum check failed (Zi)");
        return false;
      }

      // Validate TX amount burnt/mint for conversions
      if (strSource != strDest) {

        if ((version < HF_VERSION_USE_COLLATERAL) && (tx_type == tt::XASSET_TO_XUSD || tx_type == tt::XUSD_TO_XASSET)) {
          // Wallets must append the burnt fee for xAsset conversions to the amount_burnt.
          // So we subtract that from amount_burnt and validate only the actual coversion amount because
          // fees are not converted. They are just burned.

          // calculate the burnt fee. Should be the 80% of the offshoreFee
          boost::multiprecision::uint128_t fee_128 = rv.txnOffshoreFee;
          boost::multiprecision::uint128_t burnt_fee = (fee_128 * 4) / 5;

          // subtract it from amount burnt
          amount_burnt -= (uint64_t)burnt_fee;
        }

        // m = sum of all masks of inputs
        // n = sum of masks of change + collateral outputs
        // rv.maskSums[0] = m
        // rv.maskSums[1] = n
        // The value the current sumC is C = xG + aH where 
        // x = m - n, a = actual converted amount(burnt), and G, H are constants

        // add the n back to x, so x = m in calculation C = xG + aH
        // but we can't add it directly. So first calculate the C for n(mask) and 0(amount)
        key C_n;
        genC(C_n, rv.maskSums[1], 0);
        key C_burnt = addKeys(sumC, C_n);

        // Now, x actually should be rv.maskSums[0]
        // so if we calculate a C with rv.maskSums[0] and amount_burnt, C should be same as C_burnt
        key pseudoC_burnt;
        genC(pseudoC_burnt, rv.maskSums[0], amount_burnt);

        // check whether they are equal
        if (!equalKeys(C_burnt, pseudoC_burnt)) {
          LOG_PRINT_L1("Tx amount burnt/minted validation failed.");
          return false;
        }
      }

      // validate the collateral
      if ((version >= HF_VERSION_USE_COLLATERAL)) {
        if (tx_type == tt::OFFSHORE) {
          // get collateral commitment
          key C_col = rv.outPk[collateral_indices[0]].mask;

          // calculate needed commitment
          key pseudoC_col;
          genC(pseudoC_col, rv.maskSums[2], amount_collateral);

          if (!equalKeys(pseudoC_col, C_col)) {
            LOG_ERROR("Offshore collateral verification failed.");
            return false;
          }
        }

        if (tx_type == tt::ONSHORE) {
          // calculate needed commitment
          key col_C;
          genC(col_C, rv.maskSums[2], amount_collateral);
          
          // try to match with actual col ouput
          if (!equalKeys(col_C, rv.outPk[collateral_indices[0]].mask)) {
            LOG_PRINT_L1("Onshore collateral check failed.");
            return false;
          }

          // check inputs == outputs
          key sumColOut = addKeys(rv.outPk[collateral_indices[0]].mask, rv.outPk[collateral_indices[1]].mask);
          if (!equalKeys(sumColOut, sumColIns)) {
            LOG_PRINT_L1("Onshore collateral inputs != outputs");
            return false;
          }
        }
      }

      for (size_t i = 0; i < rv.p.bulletproofs.size(); i++)
        proofs.push_back(&rv.p.bulletproofs[i]);
    
      if (!proofs.empty() && !verBulletproof(proofs))
      {
        LOG_PRINT_L1("Aggregate range proof verified failed");
        return false;
      }
      
      return true;
    }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (const std::exception &e)
    {
      LOG_PRINT_L1("Error in verRctSemanticsSimple: " << e.what());
      return false;
    }
    catch (...)
    {
      LOG_PRINT_L1("Error in verRctSemanticsSimple, but not an actual exception");
      return false;
    }
  }
  
  // yC = constant for USD/XHV exchange rate
  // Ci = pseudoOuts[i] *** Ci & Di are MUTUALLY EXCLUSIVE
  // fcG' = fee in XHV = 0
  // C'k = outPk[k].mask
  // yD = constant for XHV/USD exchange rate (1/yC)
  // Di = pseudoOuts[i] *** Ci & Di are MUTUALLY EXCLUSIVE
  // fdG' = fee in USD = 0
  // D'k = outPk_usd[k].mask
  //
  //ver RingCT simple
  //assumes only post-rct style inputs (at least for max anonymity)
  bool verRctSemanticsSimple(
    const rctSig& rv, 
    const offshore::pricing_record& pr, 
    const cryptonote::transaction_type& type,
    const std::string& strSource, 
    const std::string& strDest
  ){

    try
    {
      PERF_TIMER(verRctSemanticsSimple);

      tools::threadpool& tpool = tools::threadpool::getInstance();
      tools::threadpool::waiter waiter;
      std::deque<bool> results;
      std::vector<const Bulletproof*> proofs;
      size_t max_non_bp_proofs = 0, offset = 0;

      CHECK_AND_ASSERT_MES(rv.type == RCTTypeSimple || rv.type == RCTTypeBulletproof || rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN,
                           false, "verRctSemanticsSimple called on non simple rctSig");
        
      const bool bulletproof = is_rct_bulletproof(rv.type);
      if (bulletproof)
      {
        CHECK_AND_ASSERT_MES(rv.outPk.size() == n_bulletproof_amounts(rv.p.bulletproofs), false, "Mismatched sizes of outPk and bulletproofs");
        if ((rv.type == RCTTypeCLSAG) || (rv.type == RCTTypeCLSAGN))
        {
          CHECK_AND_ASSERT_MES(rv.p.MGs.empty(), false, "MGs are not empty for CLSAG");
          CHECK_AND_ASSERT_MES(rv.p.pseudoOuts.size() == rv.p.CLSAGs.size(), false, "Mismatched sizes of rv.p.pseudoOuts and rv.p.CLSAGs");
        }
        else
        {
          CHECK_AND_ASSERT_MES(rv.p.CLSAGs.empty(), false, "CLSAGs are not empty for MLSAG");
          CHECK_AND_ASSERT_MES(rv.p.pseudoOuts.size() == rv.p.MGs.size(), false, "Mismatched sizes of rv.p.pseudoOuts and rv.p.MGs");
        }
        CHECK_AND_ASSERT_MES(rv.pseudoOuts.empty(), false, "rv.pseudoOuts is not empty");
      }
      else
      {
        CHECK_AND_ASSERT_MES(rv.outPk.size() == rv.p.rangeSigs.size(), false, "Mismatched sizes of outPk and rv.p.rangeSigs");
        CHECK_AND_ASSERT_MES(rv.pseudoOuts.size() == rv.p.MGs.size(), false, "Mismatched sizes of rv.pseudoOuts and rv.p.MGs");
        CHECK_AND_ASSERT_MES(rv.p.pseudoOuts.empty(), false, "rv.p.pseudoOuts is not empty");
      }
      CHECK_AND_ASSERT_MES(rv.outPk.size() == rv.ecdhInfo.size(), false, "Mismatched sizes of outPk and rv.ecdhInfo");
      CHECK_AND_ASSERT_MES(std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strSource) != offshore::ASSET_TYPES.end(), false, "Invalid Source Asset!");
      CHECK_AND_ASSERT_MES(std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strDest) != offshore::ASSET_TYPES.end(), false, "Invalid Dest Asset!");
      CHECK_AND_ASSERT_MES(type != cryptonote::transaction_type::UNSET, false, "Invalid transaction type.");
      if (strSource != strDest) {
        CHECK_AND_ASSERT_MES(!pr.empty(), false, "Empty pr found for a conversion tx");
      }
      
      if (!bulletproof)
        max_non_bp_proofs += rv.p.rangeSigs.size();

      results.resize(max_non_bp_proofs);

      const keyV &pseudoOuts = bulletproof ? rv.p.pseudoOuts : rv.pseudoOuts;

      // OUTPUTS SUMMED FOR EACH COLOUR
      key zerokey = rct::identity();
      // Zi is intentionally set to a different value to zerokey, so that if a bug is introduced in the later logic, any comparison with zerokey will fail
      key Zi = scalarmultH(d2h(1));

      // Calculate sum of all C'
      rct::keyV masks(rv.outPk.size());
      for (size_t i = 0; i < rv.outPk.size(); i++) {
        masks[i] = rv.outPk[i].mask;
      }
      key sumOutpks = addKeys(masks);
      DP(sumOutpks);

      // Calculate sum of all D'
      rct::keyV masks_usd(rv.outPk_usd.size());
      for (size_t i = 0; i < rv.outPk_usd.size(); i++) {
        masks_usd[i] = rv.outPk_usd[i].mask;
      }
      key sumOutpks_usd = addKeys(masks_usd);
      DP(sumOutpks_usd);

      // Calculate sum of all E' (xAssets)
      rct::keyV masks_xasset(rv.outPk_xasset.size());
      for (size_t i = 0; i < rv.outPk_xasset.size(); i++) {
        masks_xasset[i] = rv.outPk_xasset[i].mask;
      }
      key sumOutpks_xasset = addKeys(masks_xasset);
      DP(sumOutpks_xasset);

      // FEES FOR EACH COLOUR
      const key txnFeeKey = scalarmultH(d2h(rv.txnFee));
      const key txnOffshoreFeeKey = scalarmultH(d2h(rv.txnOffshoreFee));
      const key txnFeeKey_usd = scalarmultH(d2h(rv.txnFee_usd));
      const key txnOffshoreFeeKey_usd = scalarmultH(d2h(rv.txnOffshoreFee_usd));
      const key txnFeeKey_xasset = scalarmultH(d2h(rv.txnFee_xasset));
      const key txnOffshoreFeeKey_xasset = scalarmultH(d2h(rv.txnOffshoreFee_xasset));

      /*
        offshore TX:
        sumPseudoOuts = addKeys(pseudoOuts); (total of inputs)
        sumPseudoOuts_usd = zerokey; (no input usd amount)

        sumXHV = total_output_value_in_XHV (after subtracting fees)
        sumUSD = -total_output_value_in_USD

        D_scaled = sumUSD 
        yC_invert = 1 / exchange_rate_in_usd
        D_final = -total_output_value_in_XHV
        Zi = total_output_value_in_XHV - total_output_value_in_XHV = 0; 


        XUSD -> XASSET TX:
        sumPseudoOuts_usd = total_input_in_usd
        sumPseudoOuts_xasset = zerokey; (no input xasset amount)


        sumUSD = total_output_value_in_USD (after subtracting fees)
        sumXASSET = -total_output_value_in_XASSET (without fees)

        D_scaled = sumXASSET
        y = exchange_rate_in_usd
        D_final = sumXASSET * 1/ exchange_rate_in_usd = -total_output_value_in_USD
        Zi = sumUSD + D_final = 0
      */
      using tx_type = cryptonote::transaction_type;
      key sumPseudoOuts = (strSource == "XHV") ? addKeys(pseudoOuts) : zerokey;
      key sumPseudoOuts_usd = (strSource == "XUSD") ? addKeys(pseudoOuts) : zerokey;
      key sumPseudoOuts_xasset = (strSource != "XHV" && strSource != "XUSD") ? addKeys(pseudoOuts) : zerokey;
        
      DP(sumPseudoOuts);
      DP(sumPseudoOuts_usd);
      DP(sumPseudoOuts_xasset);

      // C COLOUR
      key sumXHV;
      // Remove the fees
      subKeys(sumXHV, sumPseudoOuts, txnFeeKey);
      subKeys(sumXHV, sumXHV, txnOffshoreFeeKey);
      subKeys(sumXHV, sumXHV, sumOutpks);

      // Variant COLOUR (C or D depending on the direction of the transaction)
      key sumUSD;
      // Remove the fees
      subKeys(sumUSD, sumPseudoOuts_usd, txnFeeKey_usd);
      subKeys(sumUSD, sumUSD, txnOffshoreFeeKey_usd);
      subKeys(sumUSD, sumUSD, sumOutpks_usd);

      // D COLOUR
      key sumXASSET;
      // Remove the fees
      subKeys(sumXASSET, sumPseudoOuts_xasset, txnFeeKey_xasset);
      subKeys(sumXASSET, sumXASSET, txnOffshoreFeeKey_xasset);
      subKeys(sumXASSET, sumXASSET, sumOutpks_xasset);

      // NEAC: attempt to only calculate forward
      // CALCULATE Zi
      if (type == tx_type::OFFSHORE) {
        key D_scaled = scalarmultKey(sumUSD, d2h(COIN));
        key yC_invert = invert(d2h(pr.unused1));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumXHV, D_final);
      } else if (type == tx_type::ONSHORE) {
        key C_scaled = scalarmultKey(sumXHV, d2h(pr.unused1));
        key yD_invert = invert(d2h(COIN));
        key C_final = scalarmultKey(C_scaled, yD_invert);
        Zi = addKeys(C_final, sumUSD);
      } else if (type == tx_type::OFFSHORE_TRANSFER) {
        Zi = addKeys(sumXHV, sumUSD);
      } else if (type == tx_type::XUSD_TO_XASSET) {
        key D_scaled = scalarmultKey(sumXASSET, d2h(COIN));
        key yC_invert = invert(d2h(pr[strDest]));
        key D_final = scalarmultKey(D_scaled, yC_invert);
        Zi = addKeys(sumUSD, D_final);
      } else if (type == tx_type::XASSET_TO_XUSD) {
        key C_scaled = scalarmultKey(sumUSD, d2h(pr[strSource]));
        key yD_invert = invert(d2h(COIN));
        key C_final = scalarmultKey(C_scaled, yD_invert);
        Zi = addKeys(C_final, sumXASSET);
      } else if (type == tx_type::XASSET_TRANSFER) {
        Zi = addKeys(sumUSD, sumXASSET);
      } else if (type == tx_type::TRANSFER) {
        Zi = addKeys(sumXHV, sumUSD);
      } else {
        LOG_PRINT_L1("Invalid transaction type specified");
        return false;
      }

      //check Zi == 0
      if (!equalKeys(Zi, zerokey)) {
        LOG_PRINT_L1("Sum check failed (Zi)");
        return false;
      }

      if (bulletproof)
      {
        for (size_t i = 0; i < rv.p.bulletproofs.size(); i++)
          proofs.push_back(&rv.p.bulletproofs[i]);
      }
      else
      {
        for (size_t i = 0; i < rv.p.rangeSigs.size(); i++)
          tpool.submit(&waiter, [&, i, offset] { results[i+offset] = verRange(rv.outPk[i].mask, rv.p.rangeSigs[i]); });
        offset += rv.p.rangeSigs.size();
      }
    
      if (!proofs.empty() && !verBulletproof(proofs))
      {
        LOG_PRINT_L1("Aggregate range proof verified failed");
        return false;
      }
      waiter.wait(&tpool);
      
      for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i]) {
          LOG_PRINT_L1("Range proof verified failed for proof " << i);
          return false;
        }
      }
      
      return true;
    }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (const std::exception &e)
    {
      LOG_PRINT_L1("Error in verRctSemanticsSimple: " << e.what());
      return false;
    }
    catch (...)
    {
      LOG_PRINT_L1("Error in verRctSemanticsSimple, but not an actual exception");
      return false;
    }
  }
  
  //ver RingCT simple
  //assumes only post-rct style inputs (at least for max anonymity)
  bool verRctNonSemanticsSimple(const rctSig & rv) {
    try
    {
      PERF_TIMER(verRctNonSemanticsSimple);

      CHECK_AND_ASSERT_MES(rv.type == RCTTypeSimple || rv.type == RCTTypeBulletproof || rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3,
          false, "verRctNonSemanticsSimple called on non simple rctSig");
      const bool bulletproof = is_rct_bulletproof(rv.type);
      // semantics check is early, and mixRing/MGs aren't resolved yet
      if (bulletproof)
        CHECK_AND_ASSERT_MES(rv.p.pseudoOuts.size() == rv.mixRing.size(), false, "Mismatched sizes of rv.p.pseudoOuts and mixRing");
      else
        CHECK_AND_ASSERT_MES(rv.pseudoOuts.size() == rv.mixRing.size(), false, "Mismatched sizes of rv.pseudoOuts and mixRing");

      const size_t threads = std::max(rv.outPk.size(), rv.mixRing.size());

      std::deque<bool> results(threads);
      tools::threadpool& tpool = tools::threadpool::getInstance();
      tools::threadpool::waiter waiter;

      const keyV &pseudoOuts = bulletproof ? rv.p.pseudoOuts : rv.pseudoOuts;

      const key message = get_pre_mlsag_hash(rv, hw::get_device("default"));

      results.clear();
      results.resize(rv.mixRing.size());
      for (size_t i = 0 ; i < rv.mixRing.size() ; i++) {
        tpool.submit(&waiter, [&, i] {
        if ((rv.type == RCTTypeCLSAG) || (rv.type == RCTTypeCLSAGN) || (rv.type == RCTTypeHaven2) || (rv.type == RCTTypeHaven3))
            {
                results[i] = verRctCLSAGSimple(message, rv.p.CLSAGs[i], rv.mixRing[i], pseudoOuts[i]);
            }
            else
                results[i] = verRctMGSimple(message, rv.p.MGs[i], rv.mixRing[i], pseudoOuts[i]);
        });
      }
      waiter.wait(&tpool);

      for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i]) {
          LOG_PRINT_L1("verRctMGSimple/verRctCLSAGSimple failed for input " << i);
          return false;
        }
      }

      return true;
    }
    // we can get deep throws from ge_frombytes_vartime if input isn't valid
    catch (const std::exception &e)
    {
      LOG_PRINT_L1("Error in verRctNonSemanticsSimple: " << e.what());
      return false;
    }
    catch (...)
    {
      LOG_PRINT_L1("Error in verRctNonSemanticsSimple, but not an actual exception");
      return false;
    }
  }

  //RingCT protocol
  //genRct: 
  //   creates an rctSig with all data necessary to verify the rangeProofs and that the signer owns one of the
  //   columns that are claimed as inputs, and that the sum of inputs  = sum of outputs.
  //   Also contains masked "amount" and "mask" so the receiver can see how much they received
  //verRct:
  //   verifies that all signatures (rangeProogs, MG sig, sum inputs = outputs) are correct
  //decodeRct: (c.f. https://eprint.iacr.org/2015/1098 section 5.1.1)
  //   uses the attached ecdh info to find the amounts represented by each output commitment 
  //   must know the destination private key to find the correct amount, else will return a random number    
  xmr_amount decodeRct(const rctSig & rv, const key & sk, unsigned int i, key & mask, hw::device &hwdev) {
    CHECK_AND_ASSERT_MES(rv.type == RCTTypeFull, false, "decodeRct called on non-full rctSig");
    CHECK_AND_ASSERT_THROW_MES(i < rv.ecdhInfo.size(), "Bad index");
    CHECK_AND_ASSERT_THROW_MES(rv.outPk.size() == rv.ecdhInfo.size(), "Mismatched sizes of rv.outPk and rv.ecdhInfo");

    //mask amount and mask
    ecdhTuple ecdh_info = rv.ecdhInfo[i];
    hwdev.ecdhDecode(ecdh_info, sk, rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2);
    mask = ecdh_info.mask;
    key amount = ecdh_info.amount;
    key C = rv.outPk[i].mask;
    DP("C");
    DP(C);
    key Ctmp;
    CHECK_AND_ASSERT_THROW_MES(sc_check(mask.bytes) == 0, "warning, bad ECDH mask");
    CHECK_AND_ASSERT_THROW_MES(sc_check(amount.bytes) == 0, "warning, bad ECDH amount");
    addKeys2(Ctmp, mask, amount, H);
    DP("Ctmp");
    DP(Ctmp);
    if (equalKeys(C, Ctmp) == false) {
        CHECK_AND_ASSERT_THROW_MES(false, "warning, amount decoded incorrectly, will be unable to spend");
    }
    return h2d(amount);
  }

  xmr_amount decodeRct(const rctSig & rv, const key & sk, unsigned int i, hw::device &hwdev) {
    key mask;
    return decodeRct(rv, sk, i, mask, hwdev);
  }

  xmr_amount decodeRctSimple(const rctSig & rv, const key & sk, unsigned int i, key &mask, hw::device &hwdev) {
    CHECK_AND_ASSERT_MES(rv.type == RCTTypeSimple || rv.type == RCTTypeBulletproof || rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3, false, "decodeRct called on non simple rctSig");
    CHECK_AND_ASSERT_THROW_MES(i < rv.ecdhInfo.size(), "Bad index");
    CHECK_AND_ASSERT_THROW_MES(rv.outPk.size() == rv.ecdhInfo.size(), "Mismatched sizes of rv.outPk and rv.ecdhInfo");

    //mask amount and mask
    ecdhTuple ecdh_info = rv.ecdhInfo[i];
    hwdev.ecdhDecode(ecdh_info, sk, rv.type == RCTTypeBulletproof2 || rv.type == RCTTypeCLSAG || rv.type == RCTTypeCLSAGN || rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3);
    mask = ecdh_info.mask;
    key amount = ecdh_info.amount;
    key C;
    if (rv.type == RCTTypeHaven2 || rv.type == RCTTypeHaven3) {
      CHECK_AND_ASSERT_THROW_MES(!equalKeys(rct::identity(),rv.outPk[i].mask), "warning, bad outPk mask");
      C = rv.outPk[i].mask;
    } else {
      if (!equalKeys(rct::identity(),rv.outPk[i].mask)) C = rv.outPk[i].mask;
      else if (!equalKeys(rct::identity(),rv.outPk_usd[i].mask)) C = rv.outPk_usd[i].mask;
      else if (!equalKeys(rct::identity(),rv.outPk_xasset[i].mask)) C = rv.outPk_xasset[i].mask;
    }
    DP("C");
    DP(C);
    key Ctmp;
    CHECK_AND_ASSERT_THROW_MES(sc_check(mask.bytes) == 0, "warning, bad ECDH mask");
    CHECK_AND_ASSERT_THROW_MES(sc_check(amount.bytes) == 0, "warning, bad ECDH amount");
    addKeys2(Ctmp, mask, amount, H);
    DP("Ctmp");
    DP(Ctmp);
    if (equalKeys(C, Ctmp) == false) {
      CHECK_AND_ASSERT_THROW_MES(false, "warning, amount decoded incorrectly, will be unable to spend");
    }
    return h2d(amount);
  }

  xmr_amount decodeRctSimple(const rctSig & rv, const key & sk, unsigned int i, hw::device &hwdev) {
    key mask;
    return decodeRctSimple(rv, sk, i, mask, hwdev);
  }

  bool signMultisigMLSAG(rctSig &rv, const std::vector<unsigned int> &indices, const keyV &k, const multisig_out &msout, const key &secret_key) {
    CHECK_AND_ASSERT_MES(rv.type == RCTTypeFull || rv.type == RCTTypeSimple || rv.type == RCTTypeBulletproof || rv.type == RCTTypeBulletproof2,
        false, "unsupported rct type");
    CHECK_AND_ASSERT_MES(indices.size() == k.size(), false, "Mismatched k/indices sizes");
    CHECK_AND_ASSERT_MES(k.size() == rv.p.MGs.size(), false, "Mismatched k/MGs size");
    CHECK_AND_ASSERT_MES(k.size() == msout.c.size(), false, "Mismatched k/msout.c size");
    CHECK_AND_ASSERT_MES(rv.p.CLSAGs.empty(), false, "CLSAGs not empty for MLSAGs");
    if (rv.type == RCTTypeFull)
    {
      CHECK_AND_ASSERT_MES(rv.p.MGs.size() == 1, false, "MGs not a single element");
    }
    for (size_t n = 0; n < indices.size(); ++n) {
      CHECK_AND_ASSERT_MES(indices[n] < rv.p.MGs[n].ss.size(), false, "Index out of range");
      CHECK_AND_ASSERT_MES(!rv.p.MGs[n].ss[indices[n]].empty(), false, "empty ss line");
    }

    // MLSAG: each player contributes a share to the secret-index ss: k - cc*secret_key_share
    //     cc: msout.c[n], secret_key_share: secret_key
    for (size_t n = 0; n < indices.size(); ++n) {
      rct::key diff;
      sc_mulsub(diff.bytes, msout.c[n].bytes, secret_key.bytes, k[n].bytes);
      sc_add(rv.p.MGs[n].ss[indices[n]][0].bytes, rv.p.MGs[n].ss[indices[n]][0].bytes, diff.bytes);
    }
    return true;
  }

  bool signMultisigCLSAG(rctSig &rv, const std::vector<unsigned int> &indices, const keyV &k, const multisig_out &msout, const key &secret_key) {
    CHECK_AND_ASSERT_MES((rv.type == RCTTypeCLSAG) || (rv.type == RCTTypeCLSAGN) || (rv.type == RCTTypeHaven2), false, "unsupported rct type");
    CHECK_AND_ASSERT_MES(indices.size() == k.size(), false, "Mismatched k/indices sizes");
    CHECK_AND_ASSERT_MES(k.size() == rv.p.CLSAGs.size(), false, "Mismatched k/MGs size");
    CHECK_AND_ASSERT_MES(k.size() == msout.c.size(), false, "Mismatched k/msout.c size");
    CHECK_AND_ASSERT_MES(rv.p.MGs.empty(), false, "MGs not empty for CLSAGs");
    CHECK_AND_ASSERT_MES(msout.c.size() == msout.mu_p.size(), false, "Bad mu_p size");
    for (size_t n = 0; n < indices.size(); ++n) {
      CHECK_AND_ASSERT_MES(indices[n] < rv.p.CLSAGs[n].s.size(), false, "Index out of range");
    }

    // CLSAG: each player contributes a share to the secret-index ss: k - cc*mu_p*secret_key_share
    // cc: msout.c[n], mu_p, msout.mu_p[n], secret_key_share: secret_key
    for (size_t n = 0; n < indices.size(); ++n) {
      rct::key diff, sk;
      sc_mul(sk.bytes, msout.mu_p[n].bytes, secret_key.bytes);
      sc_mulsub(diff.bytes, msout.c[n].bytes, sk.bytes, k[n].bytes);
      sc_add(rv.p.CLSAGs[n].s[indices[n]].bytes, rv.p.CLSAGs[n].s[indices[n]].bytes, diff.bytes);
    }
    return true;
  }

  bool signMultisig(rctSig &rv, const std::vector<unsigned int> &indices, const keyV &k, const multisig_out &msout, const key &secret_key) {
    if ((rv.type == RCTTypeCLSAG) || (rv.type == RCTTypeCLSAGN) || (rv.type == RCTTypeHaven2))
      return signMultisigCLSAG(rv, indices, k, msout, secret_key);
    else
      return signMultisigMLSAG(rv, indices, k, msout, secret_key);
  }

  bool accSignMultisigCLSAG(std::vector<rctSig > &rv ,rctSig & rv2, const std::vector<unsigned int> &indices) {
    CHECK_AND_ASSERT_MES(rv2.type == RCTTypeCLSAG || rv2.type == RCTTypeCLSAGN || rv2.type == RCTTypeHaven2, false, "unsupported rct type");
    CHECK_AND_ASSERT_MES(rv2.p.MGs.empty(), false, "MGs not empty for CLSAGs");
    for (size_t n = 0; n < indices.size(); ++n) {
      CHECK_AND_ASSERT_MES(indices[n] < rv2.p.CLSAGs[n].s.size(), false, "Index out of range");
    }

    rctSig &rv0=rv.at(0);
    std::vector<rctSig>::iterator ptr;
    for (size_t n = 0; n < indices.size(); ++n) {
      std::vector<rct::key> all_shares;
      for (ptr = rv.begin() + 1; ptr < rv.end() - 1; ptr++) {
        sc_add(rv2.p.CLSAGs[n].s[indices[n]].bytes, rv2.p.CLSAGs[n].s[indices[n]].bytes, ptr->p.CLSAGs[n].s[indices[n]].bytes);
        sc_sub(rv2.p.CLSAGs[n].s[indices[n]].bytes, rv2.p.CLSAGs[n].s[indices[n]].bytes, rv0.p.CLSAGs[n].s[indices[n]].bytes);
      }
    }
    return true;
  }

  bool accSignMultisigMLSAG(std::vector<rctSig > &rv ,rctSig & rv2,const std::vector<unsigned int> &indices) {
    CHECK_AND_ASSERT_MES(rv2.type == RCTTypeFull || rv2.type == RCTTypeSimple || rv2.type == RCTTypeBulletproof || rv2.type == RCTTypeBulletproof2, false, "unsupported rct type");
    CHECK_AND_ASSERT_MES(rv2.p.MGs.empty(), false, "MGs not empty for CLSAGs");
    for (size_t n = 0; n < indices.size(); ++n) {
      CHECK_AND_ASSERT_MES(indices[n] < rv2.p.CLSAGs[n].s.size(), false, "Index out of range");
    }

    rctSig &rv0=rv.at(0);
    std::vector<rctSig>::iterator ptr;
    for (size_t n = 0; n < indices.size(); ++n) {
      for (ptr = rv.begin() + 1; ptr < rv.end() - 1; ptr++) {
        sc_add(rv2.p.MGs[n].ss[indices[n]][0].bytes, rv2.p.MGs[n].ss[indices[n]][0].bytes, ptr->p.MGs[n].ss[indices[n]][0].bytes);
        sc_sub(rv2.p.MGs[n].ss[indices[n]][0].bytes, rv2.p.MGs[n].ss[indices[n]][0].bytes, rv0.p.MGs[n].ss[indices[n]][0].bytes);
      }
    }
    return true;
  }

  bool accMultisig(std::vector<rctSig > &rv,rctSig  &recvrv, const std::vector<unsigned int> &indices) {
    if (recvrv.type == RCTTypeCLSAG || recvrv.type == RCTTypeCLSAGN || recvrv.type == RCTTypeHaven2)
      return accSignMultisigCLSAG(rv, recvrv,indices);
    else
      return accSignMultisigMLSAG(rv, recvrv,indices);
  }

  bool checkBurntAndMinted(const rctSig &rv, const xmr_amount amount_burnt, const xmr_amount amount_minted, const offshore::pricing_record pr, const std::string& source, const std::string& destination, const uint8_t version) {

    if (source == "XHV" && destination == "XUSD") {
      boost::multiprecision::uint128_t xhv_128 = amount_burnt;
      boost::multiprecision::uint128_t exchange_128 = (version >= HF_PER_OUTPUT_UNLOCK_VERSION) ? std::min(pr.unused1, pr.xUSD) : pr.unused1;
      boost::multiprecision::uint128_t xusd_128 = xhv_128 * exchange_128;
      xusd_128 /= COIN;
      boost::multiprecision::uint128_t minted_128 = amount_minted;
      if (xusd_128 != minted_128) {
        LOG_PRINT_L1("Minted/burnt verification failed (offshore)");
        return false;
      }
    } else if (source == "XUSD" && destination == "XHV") {
      boost::multiprecision::uint128_t xusd_128 = amount_burnt;
      boost::multiprecision::uint128_t exchange_128 = (version >= HF_PER_OUTPUT_UNLOCK_VERSION) ? std::max(pr.unused1, pr.xUSD) : pr.unused1;
      boost::multiprecision::uint128_t xhv_128 = xusd_128 * COIN;
      xhv_128 /= exchange_128;
      boost::multiprecision::uint128_t minted_128 = amount_minted;
      if ((uint64_t)xhv_128 != minted_128) {
        LOG_PRINT_L1("Minted/burnt verification failed (onshore)");
        return false;
      }
    } else if (source == "XUSD" && destination != "XHV" && destination != "XUSD") {
      boost::multiprecision::uint128_t xusd_128 = amount_burnt;
      if (version < HF_VERSION_USE_COLLATERAL) {
        if (version >= HF_VERSION_HAVEN2) {
          xusd_128 -= ((rv.txnOffshoreFee * 4) / 5);
        } else if (version >= HF_VERSION_XASSET_FEES_V2) {
          xusd_128 -= ((rv.txnOffshoreFee_usd * 4) / 5);
        }
      }
      boost::multiprecision::uint128_t exchange_128 = pr[destination];
      boost::multiprecision::uint128_t xasset_128 = xusd_128 * exchange_128;
      xasset_128 /= COIN;
      boost::multiprecision::uint128_t minted_128 = amount_minted;
      if (xasset_128 != minted_128) {
        LOG_PRINT_L1("Minted/burnt verification failed (xusd_to_xasset)");
        return false;
      }
    } else if (source != "XHV" && source != "XUSD" && destination == "XUSD") {
      boost::multiprecision::uint128_t xasset_128 = amount_burnt;
      if (version < HF_VERSION_USE_COLLATERAL) {
        if (version >= HF_VERSION_HAVEN2) {
          xasset_128 -= ((rv.txnOffshoreFee * 4) / 5);
        } else if (version >= HF_VERSION_XASSET_FEES_V2) {
          xasset_128 -= ((rv.txnOffshoreFee_xasset * 4) / 5);
        }
      }
      boost::multiprecision::uint128_t exchange_128 = pr[source];
      boost::multiprecision::uint128_t xusd_128 = xasset_128 * COIN;
      xusd_128 /= exchange_128;
      boost::multiprecision::uint128_t minted_128 = amount_minted;
      if ((uint64_t)xusd_128 != minted_128) {
        LOG_PRINT_L1("Minted/burnt verification failed (xasset_to_xusd)");
        return false;
      }
    } else {
      LOG_PRINT_L1("Invalid request - minted/burnt values only valid for offshore/onshore/xusd_to_xasset/xasset_to_xusd TXs");
      return false;
    }

    // Must have succeeded
    return true;
  }
}
