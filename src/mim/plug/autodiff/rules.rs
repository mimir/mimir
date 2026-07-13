// const Def*erence: https://www.overleaf.com/read/gdpfxvzqpfjf


// easy readable:

/*

we define two substitutions / translations:
(and an additional one for helper functions)

we define D as transformation on closed functions
and D_A as analog for expressions in functions

We interpret a function as composition of intermediate functions (state transformers)
f(a) = (fₖ ∘ fₖ₋₁ ∘ ... ∘ f₀)(a)
The intermediate functions fₖ, fₖ₋₁, ..., f₀ correspond to the expressions.
For e, we define eλ := (fₗ ∘ ... ∘ f₀) where fₗ is the function that corresponds to e.

It holds
    eλ = λa. e
    e  = eλ a
        with a as arguments of f


closed functions:
D : (A -> B) -> (A' -> B' × (Bᵗ -> Aᵗ))
such that
f' = D f x = (f x, f*ₓ)
especially
#1 (D f x)   = f x
#2 (D f x) s = f*ₓ s = s ⋅ ∇ₓf

D is defined for $f = λ a. b$ as
D f = λ a. (b, b*) = λ a. D_A b = D bλ
    := λ a. D_A b

where D_A e = D eλ a = (e, e*)
D_A is defined for each constructor of expressions

short:
D_A : E -> (E' × (Eᵗ -> Aᵗ))
D_A e = (e⁺, e*)
    e⁺ = e

#1 (D_A e) = e
#2 (D_A e) = #2 (D e_λ a)


type rules:
(A -> B)' = A' -> B' × (Bᵗ -> Aᵗ)
Aᵗ = A



Now on to the rules:

Literal c:E
D_A c = (c, λ s. (0:A))

Application
D_A (g e) =
    let g'        = D g in
    let (e, e*)   = D_A e in
    let (y, g*_y) = g' e in
    (y, e* ∘ g*_y)

Note: we use the more modular appraoch to
just apply g' with e and separate the pullbacks
Types:
g         : E -> Y
g'        : E' -> Y' × (Yᵗ -> Eᵗ)
e         : E
e*        : Eᵗ -> Aᵗ
e* ∘ g*_y : Yᵗ -> Aᵗ

D_A (e1, ..., e_n) =
    let (e_i') = D_A ei in
    let (e_i, e_i*) = e_i' in
    ((e_1, ..., e_n),
     λ (s_1, ..., s_n). Σ_A (e_i* s_i))


For closed functions:
D mul =
    λ (a,b).
        (mul (a,b),
         λ s. (mul (s, b), mul (s, a)))

D load =


*/


/// detailed rules (now in cps)
/// we omit beta force at autodiff_type and tangent_type (will be normalized or added in type check)

/// types

.rule (T:*) :
    (%autodiff.tangent_type T) ->
    T;

.rule (A B:*) :
    (%autodiff.autodiff_type (Cn [A, Cn B])) ->
    (
        Cn [
            %autodiff.autodiff_type A,
            Cn[
                %autodiff.autodiff_type B,
                Cn[
                    %autodiff.tangent_type B,
                    Cn [%autodiff.tangent_type A]
                ]
            ]
        ]
    );

/// otherwise
/// pseudo rule
.rule (A E:*) :
    (%autodiff.inner_autodiff_type A (Cn E)) ->
    (Cn [
        %autodiff.autodiff_type E,
        Cn[
            %autodiff.tangent_type E,
            Cn [%autodiff.tangent_type A]
        ]
    ]);

/// otherwise
.rule (A:*) :
    (%autodiff.autodiff_type A) ->
    A;








/// inner_autodiff = D_A

/// literal
// first step could be inlined
cn zero_pb [[A:*, E:*],
    ret:Cn[
        %autodiff.tangent_type E,
        Cn (%autodiff.tangent_type A)
    ]] = {
    cn inner_pb [
        s  : %autodiff.tangent_type E,
        ret: Cn (%autodiff.tangent_type A)
    ] = {
        let z = %autodiff.zero (%autodiff.tangent_type A) in
        ret z
    };
    ret inner_pb
};

.rule (A:*) (n:Nat) (c:(Idx n)) :
    (%autodiff.inner_autodiff A c) ->
    (c, %cps.cps2ds_dep (...) zero_pb (A, Idx n));

/// application

cn compose [[A:*, B:*, C:*],
    ret: Cn[
        [
            Cn [B, Cn C],
            Cn [A, Cn B]
        ],
        Cn [Cn[A, Cn C]]
    ]] = {
    cn composition [
        [
            f: Cn [B, Cn C],
            g: Cn [A, Cn B]
        ],
        ret: Cn [Cn[A, Cn C]]
    ] = {
        cn res [
            a: A,
            ret: Cn C
        ] = {
            cn cont [b: B] = {
                f (b, ret)
            };
            g (a, cont)
        };
        ret res
    };
    ret composition
}

// the application needs to be inspected on meta-level

// nested higher-order application
// cps (Y=⊥) and non-cps
// TODO: insert cps2ds if necessary
.rule (A E X Y:*) (g:E->X->Y) (e:E):
    (%autodiff.inner_autodiff A (g e)) ->
    (
        let g_diff        = %autodiff.autodiff g;
        let (e_aug, e_pb) = %autodiff.inner_autodiff A e;
        g_diff e_aug
    );


/// operator application (axioms) - direct style
.rule (A E Y:*) (g:E->Y) (e:E):
    (%autodiff.inner_autodiff A (g e)) ->
    let g_diff        = %autodiff.autodiff g;
    let (e_aug, e_pb) = %autodiff.inner_autodiff A e;
    let (y, g_pb)     = g_diff e_aug;
    (
        y,
        %cps.cps2ds_dep ... (%cps.cps2ds_dep ... compose ...) (e_pb,g_pb)
    );

/// closed function application
.rule (A E Y:*) (g:Cn[E,Cn Y]) (args:[E,Cn Y]):
    (%autodiff.inner_autodiff A (g args)) ->
    let g_diff            = %autodiff.autodiff g;
    let aug_args,         = %autodiff.inner_autodiff A args;
    let (aug_e, aug_cont) = aug_args;
    let e_pb              = ... // from augmentation
    // aug_args_S* #1
    // or aug_args* (0:...,1:...)
    cn g_ret_cont [
        y: %autodiff.autodiff_type Y,
        g_pb: Cn [
            %autodiff.tangent_type Y,
            Cn [%autodiff.tangent_type E]
        ]
    ] = {
        aug_cont (
            y,
            %cps.cps2ds_dep ... (%cps.cps2ds_dep ... compose ...) (e_pb,g_pb)
        )
    };
    (
        g_diff (aug_e, g_ret_cont),
        _
    );

/// open function application (continuation - if, ret, ...)
.rule (A E:*) (f:Cn E) (e:E):
    (%autodiff.inner_autodiff A (f e)) ->
    let f_diff        = %autodiff.autodiff f;
    let (e_aug, e_pb) = %autodiff.inner_autodiff A e;
    (f_diff (e_aug, e_pb), _);
    ;


/// tuple
// TODO: correct syntax?
.rule (A:*) (n:Nat) (ET:«n; *») (t:«i:n; ET#i»):
    (%autodiff.inner_autodiff A t) ->
    let t_diff = ‹i:n; %autodiff.inner_autodiff A (t#i) ›;
    let t_aug = ‹i:n; (t_diff#i)#(ff) ›;
    cn t_pb [
        s: «i:n; %autodiff.tangent_type (ET#i) »,
        ret: Cn (%autodiff.tangent_type A)
    ] = {
        ret (%autodiff.sum (n, %autodiff.tangent_type A) (
            ‹i:n; %cps.cps2ds_dep ... ((t_diff#i)#(tt)) (s#i) ›
        ))
    };
    (
        t_aug,
        t_pb
    );

/// tuple shadow
.rule (A:*) (n:Nat) (ET:«n; *») (t:«i:n; ET#i»):
    (%autodiff.inner_shadow_autodiff A t) ->
    let t_diff = ‹i:n; %autodiff.inner_autodiff A (t#i) ›;
    ‹i:n; (t_diff#i)#(tt) ›;


/// projection
// needs meta reasoning (like app)
.rule (A:*) (n:Nat) (ET:«n; *») (t:«i:n; ET#i») (j:(Idx n)):
    (%autodiff.inner_autodiff A (t#j)) ->
    let (t_aug, t_pb) = %autodiff.inner_autodiff A t;
    let (j_aug, _) = %autodiff.inner_autodiff A j;
    let t_pb_S = %autodiff.inner_shadow_autodiff A t;
    (
        t_aug,
        t_pb_S#j_aug
    );

// if no shadow pb
.rule (A:*) (n:Nat) (ET:«n; *») (t:«i:n; ET#i») (j:(Idx n)):
    (%autodiff.inner_autodiff A (t#j)) ->
    let (t_aug, t_pb) = %autodiff.inner_autodiff A t;
    let (j_aug, _) = %autodiff.inner_autodiff A j;
    cn e_pb [
        s: %autodiff.tangent_type (ET#j),
        ret: Cn (%autodiff.tangent_type A)
    ] = {
        let s_t = insert (
                %autodiff.zero («i:n; %autodiff.tangent_type (ET#i)»),
                j_aug,
                s
            );
        t_pb (s_t,ret)
    };
    (
        t_aug,
        e_pb
    );



/// glue code:
/// - var association
/// - open continuation association (pb args)








/// autodiff (closed axioms) = D

/// automatically replaced
/// add
.rule:
    (%autodiff.autodiff (%core.wrap.add)) ->
    internal_diff_core_wrap_add;
/// mul
.rule:
    (%autodiff.autodiff (%core.wrap.mul)) ->
    internal_diff_core_wrap_mul;
// cmp
.rule:
    (%autodiff.autodiff (%core.icmp.xYgLE)) ->
    internal_diff_core_icmp_xYgLE;

/*
simple matrix

e = M × N
e* = λS. M*(S × Nᵀ) + N*(Mᵀ × S)
×* = λS. (S × Nᵀ, Mᵀ × S)

transpose
transpose* := λ S. Sᵀ

sum' = λ s. unit s
unit' = λ S. sum S

*/



/// glue code:
/// - function creation, association
/// - variable association








/// Problem Pullback
/// load, store, general matrix

/*
Application
D_A (g e) =
    let g'        = D g in
    let (e, e*)   = D_A e in
    let (y, g*_y) = g' e in
    (y, e* ∘ g*_y)
*/

/// load
// load see autodiff.mim line 250
// load deriv:
//   load pb from shadow into pointer pb holder (non-local)
//   load value, provide local id pb


/*

idea: (assume inner pointer for now)
  1. augment arguments; shadow pointer exists
  2. construct load*
    2.1 set p* using current memory (from args) and shadow pointer
  3. compose with args* ∋ p*


  load' := λ (m_0, p).
    let (m_1, p^*) = load (m_0, p^*_S) in
    let (m_2, v) = load (m_1, p) in
    ((m_2, v),
      λ (s_{m_0}, s_v).
      let (s_{m_1}, s_p : Ptr(V^t)) = malloc s_{m_0} in
      let s_{m_2} = store (s_{m_1}, s_p, s_v) in
      (s_{m_2}, s_p)
    )

  store' := λ (m_0, p, v).
    let m_1 = store (m_0, p_S^*, v*) in
    let m_2 = store (m_1, p, v) in
    (m_2,
      λ s_{m_0}. (s_{m_0}, \vec{0} : Ptr(V^t), \vec{0} : V^t)
    )

// store only has side effects



    Alternative:
    diff of load is value effect => not in pointer but during load & in value

  load' := λ (m_0, p).
    let (m_1, v^*) = load (m_0, p^*_S) in
                                ^____
    let (m_2, v) = load (m_1, p) in
    ((m_2, v),
      λ (s_{m_0}, s_v).
      let (s_{m_1}, s_p : Ptr(V^t)) = malloc s_{m_0} in
      let s_pv = v^* s_v in
      let s_{m_2} = store (s_{m_1}, s_p, s_pv) in
      (s_{m_2}, s_p)
    )

    (same as above)
  store' := λ (m_0, p, v).
    let m_1 = store (m_0, p_S^*, v*) in
                          ^________
    let m_2 = store (m_1, p, v) in
    (m_2,
      λ s_{m_0}. (s_{m_0}, \vec{0} : Ptr(V^t), \vec{0} : V^t)
    )


    Solutions:
        non-local handling
        find correct formulation
        take (shadow) pullbacks as additional arguments
        handle (some) apps together with the calling function

*/


.rule:
    (%autodiff.autodiff (%mem.load)) ->
    load_deriv;

/*

*/












/*
more ideas about different stuff



Matrix:

zip f (M,N)

zip' f' (M,N) = ...
    zip* f'

zip*:
    λ S.
    zip f' (M,N)
    |> map snd
    |> zip apply S
    |> split

zip*:
    λ S. split(
        map_reduce
            (λ (m,n,s).
                let (_, f*) = f' (m,n);
                f* s
            )
            (M,N,S)
    )


N = M1 × M2
N = mR + mul (M1,M2)
ik           ij  jk

// TODO: use mul'
//   mul* = λs. (bs,as)
M1* = mR + mul (S, M2)
ij             ik  kj
M2* = mR + mul (M1, S)
jk              ji  ik



N' = mR f' M1...Mn
(N,N_s*) = split N'
N* = λS. split (mR ... NS* S)

Mi : Mat (Ei)
N: Mat(Y)
f: E1...En -> Y
N' : Mat(Y × Y -> E1...En)
N_S* : Nat(Y->E1...En)
N*: Mat S -> [Mat Ei]

TODO:
does not work with reduction, dimensions


use add f1 f2 = lambda s. add (f1 s) (f2 s)
?


*/



// load : Mem * Ptr(V) -> Mem * V

/*
r = m2,v = load (m,p)
r,r* = (m2,v),r* = load' (m,p)
args = (m,p)

r* : M*V -> M*P(V)
args* : M*P(V) -> A
where args* := λ (sm, sp). m*(sm) + p*(sp)
  m* will most likely be id or zero
  r*,p* can be chosen
r* := λ (sm, sv).
  // or use other memory
  let (sm2, ps) = slot V sm in
  let (sm3) = store (sm2,(ps,sv)) in
  (sm3,sp)
r* is closed => can not argue about partial pullbacks
(but could use load arguments)
v* is necessarily partial

idea: p* := unpack ptr => sv => plug into loaded shadow pb
  question: when to load for correct order
idea: (assume inner pointer for now)
  1. augment arguments; shadow pointer exists
  2. construct load*
    2.1 set p* using current memory (from args) and shadow pointer
  3. compose with args* ∋ p*

Variant 1: Ptr(V)ᵗ = Ptr(Vᵗ)
  load' : Mem * Ptr(V) -> (Mem * V) * (Mem * V -> Mem * Ptr(V))
    // short: M*P(V) -> (M*V) * (M*V -> M*P(V))
  load' := λ m p.
    if shadow p then
        // p_S: Ptr(V -> A)
        // v* : V -> A
        let m2, v* = load m p_S in
        let m3, v = load m2 p;
        // p*: P(V) -> A
        set p* = (wrapped) v* (load ...)
        (
            (m3,v),
            // r*:
            // see above
        )
    else


Variant 2: Ptr(V)ᵗ = Vᵗ
  load' : Mem * Ptr(V) -> (Mem * V) * (Mem * V -> Mem * V)
  load' := λ m p.
    if shadow p then
        // p_S: Ptr(V -> A)
        // v* : V -> A
        let m2, v* = load m p_S in
        let m3, v = load m2 p;
        (
            (m3,v),
            λ s_m s_v.
                v* s_v
        )
    else


*/
