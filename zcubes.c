#ifdef VERIFY
#define SOFTASSERTS 1 // unlike asserts, softasserts won't be compiled unless SOFTASSERTS is define, this needs to happen before cstd.h is included
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gmp.h> 
#include "primes.h"                 // interface to primsieve and implementation of prime pipes for multi-threading/processing
#include "m64.h"                    // 64-bit Montgomery arithmetic
#include "b32.h"                    // 32-bit Barrett arithmetic (including functions for fast CRT-ing)
#include "bitmap.h"                 // simple bitmap test/set/lookup and enumeration
#define CSTD_ONCE
#include "cstd.h"                   // define SOFTASSERTS before this inlude if you want them on
#include "report.h"                 // reporting and output functions
#include "kdata.h"                  // loads cubic-reciprocity constraints for k, precomputes admissible d|k
#include "cbrts.h"                  // code for accessing precomputed cuberoots, inverses modulo small d, static cpmax, cdmin, sdmin, sdtab, ... declared here

/*
    Copyright (c) 2019-2020 Andrew R. Booker and Andrew V. Sutherland
    See LICENSE file for license details.
*/

// these globals are used both here and in zcheck.h so they need to be declared before the inlude below
static uint64_t dmax;               // must be < 2^63
static uint64_t pdmin;              // d divisible by p in [pdmin,dmax] must be prime
static uint64_t bpmin;              // d divisible by p in [bpmin,dmax] are prime and greater than zmax >> ZMANYBITS (we never call zrcheckmany for these)
static int zmaxbits;                // this can run up to ZMAXBITS
static uint128_t zmax128;           // zmax128 <= ZMAX, zmin128 is a lower bound on dmax/(2^(1/3)-1)
static long double zmaxld;          // long doubles only have 64 bits of integer precision (16 bit exponent), and we may truncate to double in certain situations
                                    // We add a fudge factor to handle this (zmaxld is zmax128*(1+2^-62) + 1
#include "zcheck.h"                 // code for testing z's in arithmetic progressions and splitting long progressions
static uint64_t *rbuf;              // local to this module

/*
    This is the main module for zcubes, which has the following command line interfaces

        zcubesx n k pmin pmax dmax zmax [options]

    where x is the empty string or one of r,v,d,p indicating reporting, verifciation, debugging, or profiling (see make file)

    If specified options is a number from 1 to 6 which will cause the program to only perform a subset of its functions (e.g. just enuemrate primes,
    just compute cuberoots mod primes, just enumerate arithmetic progressions, etc...), see report.h for a complete list of options.

    The parent process creates a prime pipe to enuemrate all primes p in [pmin,pmax] and creates n child processes to read the pripe and a separate sibling
    to feed the pipe (this sibling is the only process that will call primesieve -- this is both more efficient and reduces the memory footprint).
    The parent thread simply waits for all its children to finish, but it watches for aborts (if any child aborts due to an assert failure the parent
    will immediately log an error and kill all the children -- this should never happen but is done to prevent zombies and not waste energy).

    For all p in the interval [pmin,pmax] every d <= dmax not dividing k with largest prime factor p will be processed, meaning that we will check all
    pairs (d,z) with z <= zmax,

    The processing of the primes p is divided into six possible phases (in a large run only one or two will apply to a particular job).

        * Cached phase (small primes p whose cuberoots have already been precomputed -- this also applies to powers of p and all prime power divisors of d/p)
        * Uncached phase (cuberoots mod p > sqrt(dmax) have not been precomputed, nor have cuberoots mod d/p, but cuberoots mod prime power divisors of d/p have)
        * Cocached phase (cuberoots mod p > sqrt(dmax) have not been precomputed but cuberoots modulo d/p have)
        * Near prime phase (primes p close enough to dmax that in addition to cuberoots we have precomputed a complete table of inverses mod d/p
        * Prime phase (primes p that are so close to dmax that we must have d=p)
        * Bigprime phase (primes p for which we must have d=p and zmax/p is small enough that we know we don't want to split progressions)

    For each p, the enumeration of the admissible d <= dmax with p as its largest prime divisor proceeds in two separate steps

        * the part of d coprime to k is generated as the product of a power of p and powers of smaller primes, none of which divide k
          for d not a power of p this step is handled by the functions enumd and enumcd (enumd may call enumcd or call itself recusrively)
        * the part of d dividing k is generated by enumerating divisors d' of k that for each prime q|k satisfy v_q(d') in {0,v_q(k)}
          this step is handled by the functions prockd which calls procd for each d'

    Once both parts of d have been computed/chosen (the choice of gcd(k,d) is simply an index into a table of divisors of k), d is processed
    by the function procd immediately below, or by procdcoprime for d coprime to k, or by procdbigprime for large prime d close to zmax.
    Each of these functions will call one of zrcheckone, zrcheckafew, or zcrchecklift to check the resulting arithmetic progressions for z.
*/


#define MAXK                1000
#define IBATCH              256
#define CUBEROOT_BUFSIZE    88573   // 1+3+3^2+...+3^9+3^10, here 3^10 is max # cuberoots of k mod d for admissible k < 1000 and d < 2^63 coprime to k

// process d < 2^63 specified by (a,ki), where a is coprime to k and ki indexes an admissible factor of k (stored in kdtab)
static inline void procd (unsigned ki, uint64_t a, uint64_t za[], uint32_t ca)
{
    uint64_t d, n;
    uint32_t b, m;
    unsigned i, si, mi, b2, b7;

    softassert (verify_cuberoots_64(za,ca,a));

    softassert (ki >= 0 && ki <= kdcnt && a <= kdmax[ki]);

    d = a*kdtab[ki].d;
    if ( ! report_d (d, ca*kdtab[ki].n) ) return;

    si = sgnz_index(d);
    mi = kdtab[ki].fi;  m = k27ftab[mi].m;
    b7 = ( onezmod7(d,si) ? 7 : 1 );        // for k = +/-2 mod 7, for 3/7 d will have z=0 mod 7
    b2 = 1+(m&1&d);                         // note that m will be even whenever k is
    b = m*b2*b7;                            // b will be 162 if k=3, otherwise 9, 18 or 126

    // if we are reasonable close to zmax, just use z's mod a and b (no change in the number of arithmetic progressions but it may reduce their length)
    // the term 4-log2(ca) is meant to make us more willing to spend time lifting when we have more arithmetic progressions that can benefit
    n = fastceilboundl(zmaxld/((long double)a*b));
    if ( n <= ZSHORT || (n*ca) <= ZFEW ) {
        uint32_t zb[K27MAXN];
        struct k27frec *x = k27ftab+mi;
        uint64_t minv = x->minv[0];  softassert(minv);
        uint16_t dm = b32_red(d,m,minv);  softassert (mod3(dm) && (x->ztab[dm]||(m==k27&&dm==1)));
        uint32_t cb = x->zcnts[dm];
        uint16_t *r = k27zs + x->ztab[dm];
        for ( i = 0 ; i < cb ; i++ ) zb[i] = r[i]; 
        uint32_t ainvb = x->itab[b32_red(a,m,minv)]; softassert(b32_red(ainvb*b32_red(a,m,minv),m,minv)==1);
        if ( b2 > 1 ) {
            for ( i = 0 ; i < cb ; i++ ) if ( zb[i]&1 ) zb[i] += m; // lift to z = 0 mod 2
            if ( !(ainvb&1) ) ainvb += m;
            softassert (b32_red(ainvb*b32_red(a,b2*m,x->minv[1]),b2*m,x->minv[1])==1);
        }
        uint64_t binv = x->minv[2*(b7>1)+b2-1];
        if ( b7 > 1 ) {
            uint32_t b2m = b2*m;
            for ( i = 0 ; i < cb ; i++ ) zb[i] = crt7(zb[i],b2m,0); // lift to 0 mod 7
            ainvb = crt7 (ainvb, b2m, inv7(a));
            softassert (b32_red(ainvb*b32_red(a,b,binv),b,binv)==1);
        }
        if ( (uint128_t)a*b > zmax128 ) zrcheckone (d, si, a, za, ca, b, zb, cb, ainvb, binv);
        else zrcheckafew (d, si, a, za, ca, b, zb, cb, ainvb, binv, n);
    } else {
        // Lift progressions using cubic reciprocity constraints and auxiliary primes, then check
        zrchecklift (d, si, ki, a, za, ca);
    }
    profile_checkpoint ();  // if we are profiling and have collected enough information, this will end the run
}

// process d < 2^63 coprime to k
static inline void procdcoprime (uint64_t d, uint64_t z[], uint32_t c)
{
    uint32_t b;
    unsigned si, mi;

    softassert (verify_cuberoots_64(z,c,d));

    if ( ! report_d (d,c) ) return;

    si = sgnz_index(d);
    mi = (km1&d&1) + 2*( onezmod7(d,si) ? 1 : 0 );
    b = km[mi];  softassert(b);

    // if we are reasonable close to zmax, just use z's mod a and b (no change in the number of arithmetic progressions but it may reduce their length)
    // the term 4-log2(ca) is meant to make us more willing to spend time lifting when we have more arithmetic progressions that can benefit
    uint64_t l = fastceilboundl(zmaxld/((long double)d*b));
    if ( l <= ZSHORT || l*c <= ZFEW ) {
        uint64_t binv = kminv[mi];
        uint32_t db = b32_red(d,b,binv);
        uint32_t *zb = kmztab[mi]+db;
        uint32_t dinvb = kmitab[mi][db];  softassert (b32_red(d*dinvb,b,binv)==1);
        if ( (uint128_t)d*b > zmax128 ) zrcheckone (d, si, d, z, c, b, zb, 1, dinvb, binv);
        else zrcheckafew (d, si, d, z, c, b, zb, 1, dinvb, binv, l);
    } else {
        // Lift progressions using cubic reciprocity constraints and auxiliary primes, then check
        zrchecklift (d, si, 0, d, z, c);
    }
    profile_checkpoint ();  // if we are profiling and have collected enough information, this will end the run
}


// process large prime d < 2^63 (large means close enough to zmax that we don't want to think about lifting other than modulo b where cb=1
// this means b=162 if k=3, b=126 if k = +/- 2 mod 7 and onezmod(d,si) is set, and b=18 otherwise
static inline void procdbigprime (uint64_t d, uint64_t z[], uint32_t c, uint32_t si, uint32_t mi, uint32_t l)
{
    softassert (mi < 4 && km[mi]);
    softassert (verify_cuberoots_64(z,c,d));

    if ( ! report_d (d, c) ) return;

    uint64_t binv = kminv[mi];
    uint32_t b = km[mi], db = b32_red(d,b,binv), dinvb = kmitab[mi][db], *zb = kmztab[mi]+db;

    if ( l==1 ) zrcheckone (d, si, d, z, c, b, zb, 1, dinvb, binv);
    else zrcheckafew (d, si, d, z, c, b, zb, 1, dinvb, binv, l);
    profile_checkpoint ();  // if we are profiling and have collected enough information, this will end the run
}


// process d and all multiples d*m with m an admissible divisor of k (which is automatically coprime to d)
static inline void prockd (uint64_t d, uint64_t zd[], unsigned n)
{
    procdcoprime (d, zd, n);
    for ( int i = 1 ; d <= kdmax[i] ; i++ ) procd (i,d,zd,n);
}

// processes admissible dmin > cdmin (so d*cdmax >= dmax) with smallest prime divisor p (which may be less than cdmax)
// zd is a list of n cuberoots of k mod d, p is largest p|d, r is workspace for CRT-lifted cuberoots
static void inline enumcd (uint64_t d, uint64_t p, uint64_t zd[], uint32_t n, uint64_t *r)
{
    struct cdrec *z[IBATCH];
    struct cdrec *x;
    uint64_t ai[IBATCH];
    uint64_t dinv, R, R2, R3;
    uint64_t *s;
    uint32_t i,j,m;

    softassert (d >= cdmin);
    if ( ! (x = cdentry (p-1,d,dmax)) ) return;
    softassert((uint128_t)d*x->d <= dmax);
    softassert(x->p < p);

    if ( d < sdmin ) { dinv = m64_pinv(d); R = m64_R(d); R2 = m64_R2(R,d); R3 = m64_R3 (R2,d,dinv); } else { dinv=R=R2=R3=0; }

    for ( m = 0 ;;) { // terminates below when x hits the bottom of the cache, with x->d = 0
        if ( !x->d || m == IBATCH ) {
            if ( !m ) return;
            softassert(dinv);
            m64_inv_array (ai,ai,m,R,R2,R3,d,dinv);
            for ( i = 0 ; i < m ; i++ ) {
                uint64_t a = z[i]->d, u = a*m64_to_ui(ai[i],d,dinv) - 1, ab = a*d;
                for ( s = r, j = 0 ; j < z[i]->n ; j++ ) {
                    uint64_t nza = a-cdroots[z[i]->r+j];
                    for ( int ii = 0 ; ii < n ; ii++ ) *s++ = fcrt64(u,nza,zd[ii],ab);
                }
                prockd (ab,r,s-r);
            }
            if ( !x->d ) return;
            m = 0;
        }
        softassert((uint128_t)d*x->d <= dmax);
        softassert(x->p < p);
        if ( x->d <= sdmax ) {
            struct sdrec *y = sdtab+x->sdpi;
            softassert (y->d == x->d);
            uint64_t sdinv = y->dinv;
            uint64_t dinvsd = sdinvs[y->i+b32_red(d,y->d,sdinv)];
            for ( i = 0, s = r ; i < n ; i++ ) for ( j = 0 ; j < x->n ; j++ ) *s++ = b32_crt64 (zd[i],d,sdroots[y->r+j],y->d,dinvsd,sdinv);
            prockd (d*y->d,r,s-r);
        } else {
            ai[m] = m64_from_ui_R2 (x->d,R2,d,dinv); z[m] = x;
            m++;
        }
        for ( x-- ; x->p >= p ; x-- );
    }
}

// recursively enumerate admissible multiples of d by taking on prime powers; recursion ends with a call to enumcd
// zd is a list of n cuberoots of k mod d, p is the smallest prime divisor of d, r is workspace for CRT-lifted cuberoots
static void enumd (uint64_t d, uint64_t p, uint64_t zd[], uint32_t n, uint64_t *r)
{
    uint64_t dinv, R, R2, R3;
    uint64_t qq[IBATCH],ai[IBATCH], qz[3];
    uint32_t qpi[IBATCH], qe[IBATCH];
    uint64_t a,u,nza,ab,q,*s;
    uint32_t i,j,e,qn,m,pi;

    if ( d >= cdmin ) { enumcd (d,p,zd,n,r); return; }
    softassert (p <= cpmax || (uint128_t)d*cpmax >= dmax );
    if ( ! (pi = pimaxp (p-1,d,dmax)) ) return;
    dinv = m64_pinv(d); R = m64_R(d); R2 = m64_R2(R,d); R3 = m64_R3 (R2,d,dinv);
    
    q = cptab[pi]; e = 1;
    for ( m = 0 ;; m++ ) {  // terminates below when pi hits 0
        if ( ! pi || m == IBATCH ) {
            if ( !m ) return;
            m64_inv_array (ai,ai,m,R,R2,R3,d,dinv);
            for ( i = 0 ; i < m ; i++ ) {
                a = qq[i];  u = a*m64_to_ui(ai[i],d,dinv) - 1; ab = a*d;
                qn = cached_cuberoots_modq (qz,qpi[i],qe[i]);
                s = r;
                for ( j = 0 ; j < qn ; j++ ) { nza = a-qz[j]; for ( int ii = 0 ; ii < n ; ii++ ) *s++ = fcrt64(u,nza,zd[ii],ab); }
                prockd (ab,r,s-r);
                if ( ab >= cdmin ) enumcd (ab,cptab[qpi[i]],r,s-r,s);
                else enumd (ab,cptab[qpi[i]],r,s-r,s);
            }
            if (!pi) return;
            m = 0;
        }
        softassert((uint128_t)d*q <= dmax);
        // for small q we could look up inverses here
        qq[m] = q; qpi[m] = pi; qe[m] = e;
        ai[m] = m64_from_ui_R2 (q,R2,d,dinv);
        q *= cptab[pi]; e++;
        if ( (uint128_t)d*q > dmax ) { q = cptab[--pi]; e = 1; }
    }
}


static void precompute (uint32_t k, uint64_t pmin, uint64_t pmax)
{
    char zbuf[64];

    precompute_kdata (k, dmax);
    precompute_zchecks (k);
    precompute_cuberoots(k, pmin, pmax, dmax);
    pdmin = 1 + dmax / (kdmin ? _min(kdmin,cptab[1]) : cptab[1]);               // d >= pdmin must be prime not dividing k (and > 3)
    if ( pdmin <= k ) pdmin = k+1;
    bpmin = fastceilboundl(zmaxld/((km1&1?km2:km1)*ZSHORT));                        // for d >= bpmin we will never use zrcheckmany
    if ( bpmin <= 7 ) bpmin = 11;
    report_printf ("LIMITS:pmin=%lu:pmax%lu:dmax=%lu:zmax=%s:cpmax=%u:cqmax=%lu:cdmax=%u:cdmin=%lu:sdmin=%lu:pdmin=%lu:bpmin=%lu\n", pmin, pmax, dmax, itoa128(zbuf,zmax128), cpmax, cqmax, cdmax, cdmin, sdmin, pdmin, bpmin);
}

void allocate_private_buffers (void)
{
    rbuf = private_malloc (CUBEROOT_BUFSIZE*sizeof(*rbuf));
    zabuf[0] = private_malloc((1<<ZBUFBITS)*sizeof(*zabuf[0]));
    zabuf[1] = private_malloc((1<<ZBUFBITS)*sizeof(*zabuf[0]));
    zbbuf[0] = private_malloc((1<<ZBUFBITS)*sizeof(*zbbuf[1]));
    zbbuf[1] = private_malloc((1<<ZBUFBITS)*sizeof(*zbbuf[1]));
    bm0buf = private_malloc((1<<(BMBITS-3)));
    bm1buf = private_malloc((1<<(BMBITS-3)));
}

void free_private_buffers (void)
{
    private_free (rbuf, CUBEROOT_BUFSIZE*sizeof(*rbuf));
    private_free (zabuf[0], (1<<ZBUFBITS)*sizeof(*zabuf[0]));
    private_free (zabuf[1], (1<<ZBUFBITS)*sizeof(*zabuf[0]));
    private_free (zbbuf[0],(1<<ZBUFBITS)*sizeof(*zbbuf[1]));
    private_free (zbbuf[1], (1<<ZBUFBITS)*sizeof(*zbbuf[1]));
    private_free (bm0buf,(1<<(BMBITS-3)));
    private_free (bm1buf,(1<<(BMBITS-3)));
}

// Used when largest p|d is fixed to a single prime p0 and we are iterating over the second largest prime
// In this scenario we assume all the primes involved are cached (and smaller than sqrt(dmax))
static void process_subprimes (uint32_t p0, uint32_t *itabp0, primes_pipe_ctx_t *pipe, int jobid, uint64_t *r)
{
    uint64_t p0inv, p, q, pmax, dmax0;
    uint32_t z0[3];
    uint64_t z[3], zz[3];
    uint32_t i,j,m,n,n0,pi,qinvp0;

    pmax = pipe->end;
    assert (pmax <= p0 && p0 <= cpmax);

    pi = pimaxp (p0,1,dmax);
    if ( primes_next_prime(p0-1) != p0 ) { report_printf ("Nothing to do for nonprime p0=%u\n", p0); return; }
    if ( p0 > 1 && mod3(p0) == 1 && ! has_cuberoots_modp(K,p0) ) { report_printf ("Nothing to do, there are no cuberoots of k=%u mod p0=%u\n", K, p0); return; }
    assert (cptab[pi]==p0);

    dmax0 = dmax / p0;
    p0inv = p0 > 2 ? b32_inv (p0) : (1UL<<63);
    n0 = cached_cuberoots_modq (z,pi,1);  assert (n0>0);
    if ( ! n0 ) return;
    for ( i = 0 ; i < n0 ; i++ ) z0[i] = (uint32_t) z[i];

    p = primes_read_pipe(pipe,jobid);
    pi = pimaxp(p,1,dmax0);

    if ( pmax == p0 ) pmax--;
    for ( ; p <= pmax && p < p0 ; p = primes_read_pipe (pipe,jobid) ) {
        report_p (p);                                           // we need to report p for checkpointing purposes by report.h knows not to increment pcnt
        while ( pi <= cpcnt && cptab[pi] < p ) pi++;            // a linear scan is almost certainly faaster than using pimaxp
        if ( pi > cpcnt || cptab[pi] > p ) continue;            // this may happen if there are no cuberoots of k mod p (even when pi > cpcnt)
        for ( i=1,q=p ; (uint128_t)q*p <= dmax0 ; i++, q*= p ); // determine the largest power q of p that we need
        n = ( cached_cuberoots_e (pi) >= i ? cached_cuberoots_modq (z,pi,i) : cuberoots_modq (z,K,p,i) );   // get cuberoots mod q=p^i, cached if possible
        assert (n>0);  m = n*n0;
        for ( uint64_t pp=p ; pp < q ; pp*=p ) {
            for ( i = 0 ; i < n ; i++ ) zz[i] = z[i]%pp;        // each mod takes under 20 cycles, not worth trying to optimize
            qinvp0 = itabp0[b32_red(pp,p0,p0inv)];  softassert (b32_red((uint64_t)qinvp0*b32_red(pp,p0,p0inv),p0,p0inv)==1);
            for ( i = 0 ; i < n ; i++ ) for ( j = 0 ; j < n0 ; j++ ) r[i*n0+j] = b32_crt64(zz[i],pp,z0[j],p0,qinvp0,p0inv);
            prockd (pp*p0,r,m); enumd(pp*p0,p,r,m,r+m);         // process all d divisible by pp*p0 (prockd handles cofactors dividing k, enumd the rest)
        }
        qinvp0 = itabp0[b32_red(q,p0,p0inv)];  softassert (b32_red((uint64_t)qinvp0*b32_red(q,p0,p0inv),p0,p0inv)==1);
        for ( i = 0 ; i < n ; i++ ) for ( j = 0 ; j < n0 ; j++ ) r[i*n0+j] = b32_crt64(z[i],q,z0[j],p0,qinvp0,p0inv);
        prockd (q*p0,r,m); enumd(q*p0,p,r,m,r+m);               // process all d divisible by q*p0 (prockd handles cofactors dividing k, enumd the rest)
    }

    // if p0 pops output the pipe we need to handle d=p and d divisible by p^2
    if ( p==p0 ) {
        report_p (p0);                                          // report.h will increment pcnt for p=p0
        while ( pi <= cpcnt && cptab[pi] < p ) pi++;            // a linear scan is almost certainly faaster than using pimaxp
        for ( i=1,q=p ; (uint128_t)q*p <= dmax ; i++, q*= p );  // determine the largest power q of p0 that we need
        n = ( cached_cuberoots_e (pi) >= i ? cached_cuberoots_modq (z,pi,i) : cuberoots_modq (z,K,p0,i) );   // get cuberoots mod q=p^i, cached if possible
        assert (n>0);
        report_c (n);                                           // we do report cuberoots for p=p0
        for ( uint64_t pp=p ; pp < q ; pp*=p ) {
            for ( i = 0 ; i < n ; i++ ) zz[i] = z[i]%pp;        // each mod takes under 20 cycles, not worth trying to optimize
            prockd (pp,zz,n); if ( pp > p ) enumd(pp,p,zz,n,r); // note that here we do not call enumd for pp=p, we effectively implemented this above
        }
        prockd (q,z,n); if ( q > p ) enumd(q,p,z,n,r);          // note that here we do not call enumd for q=p, we effectively implemented this above
    }
 
    assert (p > pmax);
}

// This the main loop for each child thread (or the single main thread for n=1)
// For each p in the pipe (all p in [pmin,pmax] if we are the only core) processed all d with largest prime divisor p
static void process_primes (primes_pipe_ctx_t *pipe, int jobid, uint64_t *r)
{
    uint64_t p, q, pmax;
    uint64_t z[3], zz[3];
    uint32_t i,j,n;

    pmax = pipe->end;
    p = primes_read_pipe(pipe,jobid);
    if ( p > pmax ) return;

    // Note that the code below relies on the fact that primes_read_pipe returns 2^64-1 on end of pipe
    // So even if pipe->end < cpmax (for example), we will get a return value > cpmax at the end of the pipe

    // Phase 1: primes p <= cpmax (note cpmax >= sqrt(dmax)) which were considerd during the precomputation phase
    // For these primes we know cuberoots mod p (and some powers of p but not necessarily up to dmax, this depends on pmin)
    // We are guaranteed that for any power of a prime q < p appearing in a cofactor we will have cached cuberoots (which enumd will use)
    if ( p <= cpmax ) {
        uint32_t pi = pimaxp(pipe->start,1,dmax);
        for ( ; p <= cpmax && p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
            if ( ! report_p(p) ) continue;
            while ( pi <= cpcnt && cptab[pi] < p ) pi++;            // a linear scan is almost certainly faaster than using pimaxp
            if ( pi > cpcnt || cptab[pi] > p ) continue;            // this may happen if there are no cuberoots of k mod p (even when pi > cpcnt)
            for ( i=1, q=p ; (uint128_t)q*p <= dmax ; i++, q*= p ); // determine the largest power q of p that we need
            // we could gain perhaps a factor of two in the line below in the uncached case by lifting the cuberoots mod p we have precomputed
            // the benefit of doing this is negligible (for primes this small there are many d per p)
            n = ( cached_cuberoots_e (pi) >= i ? cached_cuberoots_modq (z,pi,i) : cuberoots_modq (z,K,p,i) );   // get cuberoots mod q=p^i, cached if possible
            assert (n>0);
            if ( ! report_c (n) ) continue;
            for ( uint64_t pp=p ; pp < q ; pp*=p ) {
                for ( i = 0 ; i < n ; i++ ) zz[i] = z[i]%pp;        // each mod takes under 20 cycles, not worth trying to optimize
                prockd (pp,zz,n); enumd(pp,p,zz,n,r);               // process all d divisible by pp  < q (prockd handles cofactors dividing k, enumd the rest)
            }
            prockd (q,z,n);  enumd(q,p,z,n,r);                      // process all d divisible by pp  < q (prockd handles cofactors dividing k, enumd the rest)
        }
    }
    if ( ! report_phase(PHASE_CACHED) ) goto done;
    if ( p > pmax ) goto done;

    // at this point we assume all primes up to cpmax >= sqrt(dmax) have been processed
    // henceforth all d will be the product of a prime p > sqrt(dmax) and a sqrt(dmax)-smooth cofactor
    assert (p > sqrt(dmax));
    assert (3*p > K);                                               // to avoid checking for p|k below we want to assume all such p are cached

    // Phase 2: primes p in (cpmax,cdmin) -- both p > sqrt(dmax) and cofactors may be uncached (for dmax < CDMAX^2 there will be no such p)
    // For these primes we compute cuberoots and then call enumd to recursively tack on powers of smaller primes (enumd will check for cached cuberoots)
    for ( softassert (p>cpmax); p < cdmin && p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
        if ( ! report_p(p) ) continue;
        n = cuberoots_modp (z,K,p);
        if ( ! n || ! report_c(n) ) continue;
        prockd (p,z,n); enumd (p,p,z,n,r);                          // process all d divisible by pp  < q (prockd handles cofactors dividing k, enumd the rest)
    }
    report_phase (PHASE_UNCACHED);
    if ( p > pmax ) goto done;

    // Phase 3: primes in [cdmin,sdmin) -- for these p all possible cofactors have cached cuberoots, but not necessarily cached inverses
    // For these we compute cuberoots and then call (inline) enumcd to tack on precomputed roots modulo all admissible cofactors d with p*d <= dmax
    for ( softassert (p>=cdmin) ; p < sdmin && p <= pmax; p = primes_read_pipe (pipe,jobid) ) {
        if ( ! report_p (p) ) continue;
        n = cuberoots_modp (z,K,p);
        if ( ! n || ! report_c (n) ) continue;
        prockd (p,z,n); enumcd (p,p,z,n,r);                         // process all d divisible by pp  < q (prockd handles cofactors dividing k, enumd the rest)
    }
    report_phase (PHASE_COCACHED);
    if ( p > pmax ) goto done;

    // Phasse 4: primes in [sdmin,pdmin) -- for these p all possible cofactors have cached cuberoots and inverses
    // For these primes we compute cuberoots and process all admissible cofactors using cached cuberoots and inverses
    struct sdrec *x;
    int pimax = sdcnt;
    for ( softassert (p>=sdmin) ; p < pdmin && p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
        if ( ! report_p(p) ) continue;
        n = cuberoots_modp (z,K,p);
        if ( !n || !report_c(n) ) continue;
        prockd (p,z,n);                                             // process all d that are p times a (possibly trivial) cofactor dividing k
        while ( pimax && (uint128_t)p*sdtab[pimax].d > dmax ) pimax--;
        for ( x = sdtab+pimax ; x > sdtab ; x-- ) {
            uint64_t dinv = x->dinv;
            uint64_t pinvb = sdinvs[x->i+b32_red(p,x->d,dinv)], *s = r;
            for ( i = 0 ; i < n ; i++ ) for ( j = 0 ; j < x->n ; j++ ) *s++ = b32_crt64 (z[i],p,sdroots[x->r+j],x->d,pinvb,dinv);
            prockd (p*x->d,r,s-r);                                  // process all d that are p*x->d times a (possibly trivial) cofactor dividing k
        }
    }
    report_phase (PHASE_NEARPRIME);
    if ( p > pmax ) goto done;

    // Phasse 5: primes in [pdmin,bpmin) -- we must have d=p, no cofactors are possible
    // For these primes we just compute cuberoots and process d=p prime using procdcoprime
    for ( softassert (p>=pdmin) ; p < bpmin && p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
        if ( ! report_p(p) ) continue;
        n = cuberoots_modp (z,K,p);
        if (! n || ! report_c(n) ) continue;
        procdcoprime (p,z,n);   // process d = p
    }
    report_phase (PHASE_PRIME);
    if ( p > pmax ) goto done;

    // Phasse 6: primes in (bpmin,pmax] -- we have d=p prime and close enough to zmax that we don't want to split arithmetic progressions in order to lift them
    // For these primes we just compute cuberoots and process d=p using procdprime (which will call zrcheckone or zrcheckafew but never zrcheckmany)
    uint32_t si, mi = km1&1, m = km[mi];                                // by default we mod km[mi] = 18 or 162 (if k=3)
    softassert (m && !(m&1));
    uint32_t l = fastceilboundl(zmaxld/((long double)p*m));                 // l = length of arithmetic progressions for current p
    softassert ( l <= ZSHORT && (uint128_t)l*p*m > zmax128 );
    uint64_t lpmax = (uint128_t)(l-1)*m*pmax > zmax128 ? fastceilboundl (zmaxld/((long double)m*(l-1))) : pmax;

    if ( mod7(K*K) != 4 ) {
        for ( softassert (p >= bpmin) ; p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
            if ( ! report_p(p) ) continue;
            n = cuberoots_modp (z,K,p);
            if ( !n || ! report_c(n) ) continue;
            si = sgnz_index(p);
            if ( p > lpmax ) { l = fastceilboundl(zmaxld/((long double)p*m)); lpmax = (uint128_t)(l-1)*m*pmax > zmax128 ? fastceilboundl (zmaxld/((long double)m*(l-1))) : pmax; }          
            procdbigprime (p,z,n,si,mi,l);
        }
    } else {
        uint32_t mi7 = mi+2, m7 = km[mi7];
        softassert (m7 && !(m7&1) && !mod7(m7));    // in fact m7=126
        uint32_t l7 = fastceilboundl(zmaxld/((long double)p*m7));                   // l = length of arithmetic progressions for current p
        uint64_t lpmax7 = (uint128_t)(l-1)*m7*pmax > zmax128 ? fastceilboundl (zmaxld/((long double)m7*(l-1))) : pmax;
        for ( softassert (p >= bpmin) ; p <= pmax ; p = primes_read_pipe (pipe,jobid) ) {
            if ( ! report_p(p) ) continue;
            n = cuberoots_modp (z,K,p);
            if ( !n || ! report_c(n) ) continue;
            si = sgnz_index(p);
            if ( (j=onezmod7(p,si)) ) {
                if ( p > lpmax7 ) { l7 = fastceilboundl(zmaxld/((long double)p*m7)); lpmax7 = (uint128_t)(l-1)*m7*pmax > zmax128 ? fastceilboundl (zmaxld/((long double)m7*(l-1))) : pmax; }
                i = mi7; j = l7;
            } else {
                if ( p > lpmax ) { l = fastceilboundl(zmaxld/((long double)p*m)); lpmax = (uint128_t)(l-1)*m*pmax > zmax128 ? fastceilboundl (zmaxld/((long double)m*(l-1))) : pmax; }
                i = mi; j = l;
            }
            procdbigprime (p,z,n,si,i,j);
        }
    }
    report_phase (PHASE_BIGPRIME);

done:
    assert (p > pmax);
}


int main (int argc, char *argv[])
{
    uint64_t pmin, pmax, start_pmin;
    uint32_t p0, *itabp0=0;
    char *s;
    int k, n, opts, cores, status;

    if ( argc < 7 ) { fprintf (stderr,"    zcubes n k pmin pmax dmax zmax [options]\n    (version %s)\n", VERSION_STRING); return 0; }

    cores = atoi(argv[1]);
    assert (cores >= 0);

    if ( profiling() && cores != 1 ) { fprintf (stderr, "Setting cores to 1 for profiling.\n"); cores = 1; }

    n = get_nprocs();
    if ( ! cores ) { cores = n; report_printf ("Using %d threads.\n", cores); }
    else { if ( cores > n ) fprintf (stderr, "WARNING: specified number of cores %d exceeds number of cores %d available\n", cores, n); }

    k = atoi(argv[2]);  if ( k < 0 || ! goodk(k) ) { fprintf (stderr, "ERROR: k=%d must be a postive integer <= 1000 congruent to 3 or 6 mod 9.\n",k); return -1; }

    dmax = strto64(argv[5]);
    if ( dmax > DMAX ) { fprintf (stderr, "ERROR: dmax = %lu cannot exceed DMAX = %lu\n", dmax, DMAX); return -1; }

    if ( (s=strchr(argv[3],'x')) ) {
        if ( memcmp(argv[3],argv[4],s+1-argv[3]) != 0 ) { fprintf (stderr, "ERROR: pmax=%s not valid for pmin=%s (if pmin=p0xq we require pmax=p0xr with r>=q)\n", argv[4], argv[3]); return -1; }
        *s = '\0'; p0 = atoi(argv[3]);
        if ( p0 < 2 ) { fprintf (stderr, "ERROR: p0=%u must be at least 2\n", p0); return -1; }
        pmin = atoi(s+1);
        pmax = atoi(argv[4]+(s+1-argv[3]));
        if ( pmax > p0 ) { fprintf (stderr, "ERROR: We must have pmax=%u*%lu <= %u*%u\n", p0, pmax, p0, p0); return -1; }
        if ( profiling() || (argc > 7 && atoi(argv[7])) ) { fprintf (stderr, "ERROR: Profiling and options are not permitted for pmin=%ux%lu pmax=%ux%lu\n", p0, pmin, p0, pmax); return -1; }
    } else {
        pmin = strto64(argv[3]);
        pmax = strto64(argv[4]);
        if ( cores > 1 && pmin == pmax && pmax <= sqrt(dmax) ) { p0 = pmin; pmin = 2; } else p0 = 1;
    }
    if ( p0 > 1 && primes_next_prime(p0-1) != p0 ) { fprintf (stderr, "WARNING: p0=%u is not prime\n", p0); }
    if ( p0 > 1 && mod3(p0) == 1 && ! has_cuberoots_modp(k,p0) ) { fprintf (stderr, "WARNING: There are no cuberoots of k=%u mod p0=%u\n", k, p0); }
    if ( p0 > 1 && !(k%p0) ) { fprintf (stderr, "ERROR: p0=%u divides k=%u, this case is not currently supported\n", p0, k); return -1; }
    if ( pmin < 2 ) pmin = 2;
    if ( pmax < pmin ) { fprintf (stderr, "ERROR: We must have pmin=%lu <= pmax=%lu and pmax > 1\n", pmin, pmax); return -1; }

    zmax128 = strto128(argv[6]);
    zmaxbits = ui128_len(zmax128);
    if ( zmaxbits > ZMAXBITS ) { fprintf (stderr, "ERROR: zmax = %s cannot exceed 2^%d.\n", argv[6], ZMAXBITS); return -1; }
    assert (zmax128 < ZMAX);
    zmaxld = (long double) (zmax128 + (zmax128>>62) + 1);   // add a fudge factor to account for the loss of precision
    assert (zmaxld > zmax128);

    if ( reporting() ) opts = argc > 7 ? atoi(argv[7]) : 0; else { opts = 0; if ( argc > 7 ) fprintf (stderr, "WARNING: Ignoring option %d with reporting off.\n", opts); }

    if ( sqrt(dmax) < p0 ) { fprintf (stderr, "ERROR: We must have p0=%u <= sqrt(dmax)=%.1f\n", p0, sqrt(dmax)); return -1; }
    if ( pmax < pmin || dmax < p0*pmax || zmax128 < dmax ) { char buf[64]; fprintf (stderr, "ERROR: We must have pmin=%lu <= pmax=%lu <= dmax=%lu <= zmax=%s\n", pmin, pmax, dmax, itoa128(buf,zmax128)); return -1; }
    long double zminld = 3.847322101863072639L*dmax;
    if ( zminld > zmaxld ) { fprintf (stderr, "WARNING: for dmax=%lud we have zminld=%Lf > zmaxld=%.0Lf, you should increase zmax or decrease dmax\n", dmax, zminld, zmaxld); if ( ! opts ) return -1; }

    output_start (cores, k, p0, pmin, pmax, dmax, zmax128, opts);
    start_pmin = report_start (cores, k, p0, pmin, pmax, dmax, zmax128, opts);
    precompute (k, p0>1?p0:pmin, p0>1?p0:pmax);
    if ( p0 > 1 ) {
        itabp0 = shared_malloc (p0*sizeof(*itabp0));
        uint32_t *w = malloc (2*p0*sizeof(*w));
        invtab32 (itabp0, p0, &p0, 1, w);
        free (w);
    }
    report_printf("Shared memory usage is %.3f MB\n", (double)shared_bytes()/(1<<20));
    assert (!private_bytes());
 
    if ( ! report_phase (PHASE_PRECOMPUTE) ) { report_end(); exit (0); }

    if ( profiling() ) {
        allocate_private_buffers (); profile_start (); process_primes (primes_create_pipe(start_pmin,pmax,0,0,0),0,rbuf); profile_end (); free_private_buffers();
        assert(0);  // we should never get here, we should terminate in the call to profile_end above
    }

    pid_t pids[cores+1];
    primes_pipe_ctx_t *pipe = primes_create_pipe (start_pmin, pmax, cores, 0, 0);
    for ( int i = 0 ; i < cores ; i++ ) {
        if ( !(pids[i]=fork()) ) {
            allocate_private_buffers();
            if ( !i ) report_printf("Private memory usage is %d * %.3f MB = %.3f MB\n", cores, (double)private_bytes()/(1<<20), (double)(cores*private_bytes())/(1<<20));
            report_job_start (i);
            if ( p0 > 1 ) process_subprimes (p0, itabp0, pipe, i, rbuf); else process_primes (pipe, i, rbuf);
            report_job_end (i);
            free_private_buffers();
            primes_close_pipe (pipe, i);
            _exit (0);
        }
        if ( pids[i] < 0 ) { for ( int j = 0 ; j < i ; j++ ) { kill (pids[j],SIGTERM); } exit (-1); }
    }
    // create a separate child to feed the rest (this is the only one that will call primesieve)
    if ( !(pids[cores]=fork()) ) {
        while (primes_feed_pipe(pipe)); // if a job aborts we may wait forever here, but parent will kill everyone if this happens
        primes_destroy_pipe (pipe);     // this will wait for our siblings to call primes_close_pipe
        _exit (0);
    }
    // if any child exits abnormally (e.g. due to an assert failure) kill them all and output an error
    while ( wait(&status) > 0 ) if ( !WIFEXITED(status) || WEXITSTATUS(status) ) {
        for ( int i = 0 ; i <= cores ; i++ ) { kill (pids[i],SIGTERM); }
        output_end (cores, k, p0, pmin, pmax, dmax, zmax128, opts, 1);
        exit (-1);
    }

    report_end ();
    if ( reporting() && argc > 7 ) {   // check for predictions specified on the command line that we want to compare against
        uint64_t pcnt=0, ccnt=0, dcnt=0, rcnt=0;
        for ( int i = 7 ; i < argc ; i++ ) {
            if ( memcmp(argv[i],"pcnt=",5) == 0 ) pcnt = strto64(argv[i]+5);
            if ( memcmp(argv[i],"ccnt=",5) == 0 ) ccnt = strto64(argv[i]+5);
            if ( memcmp(argv[i],"dcnt=",5) == 0 ) dcnt = strto64(argv[i]+5);
            if ( memcmp(argv[i],"rcnt=",5) == 0 ) rcnt = strto64(argv[i]+5);
        }
        report_comparisons (pcnt,ccnt,dcnt,rcnt);
    }
    output_end (cores, k, p0, pmin, pmax, dmax, zmax128, opts, 0);
    exit (0);
}
