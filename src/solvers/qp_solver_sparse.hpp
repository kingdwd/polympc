#ifndef QP_SOLVER_SPARSE_H
#define QP_SOLVER_SPARSE_H

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <cmath>

namespace qp_solver_sparse {

template <int _n, int _m, typename _Scalar = double>
struct QP {
    using Scalar = _Scalar;
    enum {
        n=_n,
        m=_m
    };
    Eigen::Matrix<Scalar, n, n> P;
    Eigen::Matrix<Scalar, n, 1> q;
    Eigen::Matrix<Scalar, m, n> A;
    Eigen::Matrix<Scalar, m, 1> l, u;
};

template <typename Scalar>
struct qp_sover_settings_t {
    Scalar rho = 1e-1;          /**< ADMM rho step, 0 < rho */
    Scalar sigma = 1e-6;        /**< ADMM sigma step, 0 < sigma, (small) */
    Scalar alpha = 1.0;         /**< ADMM overrelaxation parameter, 0 < alpha < 2,
                                     values in [1.5, 1.8] give good results (empirically) */
    Scalar eps_rel = 1e-3;      /**< Relative tolerance for termination, 0 < eps_rel */
    Scalar eps_abs = 1e-3;      /**< Absolute tolerance for termination, 0 < eps_abs */
    int max_iter = 1000;        /**< Maximal number of iteration, 0 < max_iter */
    int check_termination = 25; /**< Check termination after every Nth iteration, 0 (disabled) or 0 < check_termination */
    bool warm_start = false;    /**< Warm start solver, reuses previous x,z,y */
    bool adaptive_rho = false;  /**< Adapt rho to optimal estimate */
    Scalar adaptive_rho_tolerance = 5;  /**< Minimal for rho update factor, 1 < adaptive_rho_tolerance */
    int adaptive_rho_interval = 25; /**< change rho every Nth iteration, 0 < adaptive_rho_interval,
                                         set equal to check_termination to save computation  */
};

template <typename Scalar>
struct qp_solver_info_t {
    enum {
        SOLVED,
        MAX_ITER
    } status;
    int iter;
    Scalar res_prim;
    Scalar res_dual;
};

/**
 *  minimize        0.5 x' P x + q' x
 *  subject to      l <= A x <= u
 *
 *  with:
 *    x element of R^n
 *    Ax element of R^m
 */
template <typename QPType,
          template <typename, int, typename... Args> class LinearSolver = Eigen::SimplicialLDLT,
          int LinearSolver_UpLo = Eigen::Lower>
class QPSolverSparse {
public:
    enum {
        n=QPType::n,
        m=QPType::m
    };

    using qp_t = QPType;
    using Scalar = typename QPType::Scalar;
    using SpMat = Eigen::SparseMatrix<Scalar>;
    using var_t = Eigen::Matrix<Scalar, n, 1>;
    using constraint_t = Eigen::Matrix<Scalar, m, 1>;
    using dual_t = Eigen::Matrix<Scalar, m, 1>;
    using kkt_vec_t = Eigen::Matrix<Scalar, n + m, 1>;
    using kkt_mat_t = Eigen::Matrix<Scalar, n + m, n + m>;
    using settings_t = qp_sover_settings_t<Scalar>;
    using info_t = qp_solver_info_t<Scalar>;
    using linear_solver_t = LinearSolver<SpMat, LinearSolver_UpLo>;

    static constexpr Scalar RHO_MIN = 1e-6;
    static constexpr Scalar RHO_MAX = 1e+6;
    static constexpr Scalar RHO_TOL = 1e-4;
    static constexpr Scalar RHO_EQ_FACTOR = 1e+3;
    static constexpr Scalar LOOSE_BOUNDS_THRESH = 1e+16;
    static constexpr Scalar DIV_BY_ZERO_REGUL = 1e-10;

    // Solver state variables
    int iter;
    var_t x;
    constraint_t z;
    dual_t y;
    var_t x_tilde;
    constraint_t z_tilde;
    constraint_t z_prev;
    dual_t rho_vec;
    dual_t rho_inv_vec;
    Scalar rho;

    // State
    Scalar res_prim;
    Scalar res_dual;
    Scalar _max_Ax_z_norm;
    Scalar _max_Px_ATy_q_norm;

    enum {
        INEQUALITY_CONSTRAINT,
        EQUALITY_CONSTRAINT,
        LOOSE_BOUNDS
    } constr_type[m]; /**< constraint type classification */

    settings_t _settings;
    info_t _info;

    kkt_mat_t kkt_mat;
    SpMat kkt_mat_sparse;
    linear_solver_t linear_solver;

    QPSolverSparse()
    {
        x.setZero();
        z.setZero();
        y.setZero();
    }

    void solve(const qp_t &qp)
    {
        kkt_vec_t rhs, x_tilde_nu;
        bool check_termination = false;

#ifdef OSQP_PRINTING
        print_settings(_settings);
#endif
        if (!_settings.warm_start) {
            x.setZero();
            z.setZero();
            y.setZero();
        }

        constr_type_init(qp);
        rho_update(_settings.rho);

        KKT_mat_update(qp, kkt_mat);
        linear_solver.compute(kkt_mat_sparse);

        for (iter = 1; iter <= _settings.max_iter; iter++) {
            z_prev = z;

            // update x_tilde z_tilde
            form_KKT_rhs(qp, rhs);
            x_tilde_nu = linear_solver.solve(rhs);

            x_tilde = x_tilde_nu.template head<n>();
            z_tilde = z_prev + rho_inv_vec.cwiseProduct(x_tilde_nu.template tail<m>() - y);

            // update x
            x = _settings.alpha * x_tilde + (1 - _settings.alpha) * x;

            // update z
            z = _settings.alpha * z_tilde + (1 - _settings.alpha) * z_prev + rho_inv_vec.cwiseProduct(y);
            box_projection(z, qp.l, qp.u); // euclidean projection

            // update y
            y = y + rho_vec.cwiseProduct(_settings.alpha * z_tilde + (1 - _settings.alpha) * z_prev - z);

            if (_settings.check_termination != 0 && iter % _settings.check_termination == 0) {
                check_termination = true;
            } else {
                check_termination = false;
            }

            if (check_termination) {
                update_state(qp);

#ifdef OSQP_PRINTING
                print_status(qp);
#endif
                if (termination_criteria(qp)) {
                    _info.status = info_t::SOLVED;
                    break;
                }
            }

            if (_settings.adaptive_rho && iter % _settings.adaptive_rho_interval == 0) {
                if (!check_termination) {
                    // state was not yet updated
                    update_state(qp);
                }
                Scalar new_rho = rho_estimate(rho, qp);
                new_rho = fmax(RHO_MIN, fmin(new_rho, RHO_MAX));

                if (new_rho < rho / _settings.adaptive_rho_tolerance ||
                    new_rho > rho * _settings.adaptive_rho_tolerance) {
                    rho_update(new_rho);
                    KKT_mat_update(qp, kkt_mat);
                    // assumes same sparsity pattern
                    linear_solver.factorize(kkt_mat_sparse);
                }
            }
        }

        if (iter > _settings.max_iter) {
            _info.status = info_t::MAX_ITER;
        }
        _info.iter = iter;
    }

    inline const var_t& primal_solution() const { return x; }
    inline var_t& primal_solution() { return x; }

    inline const dual_t& dual_solution() const { return y; }
    inline dual_t& dual_solution() { return y; }

    inline const settings_t& settings() const { return _settings; }
    inline settings_t& settings() { return _settings; }

    inline const info_t& info() const { return _info; }
    inline info_t& info() { return _info; }

private:
    void KKT_mat_update(const qp_t &qp, kkt_mat_t& kkt)
    {
        kkt.template topLeftCorner<n, n>() = qp.P + _settings.sigma * qp.P.Identity();
        if (LinearSolver_UpLo == Eigen::Upper|Eigen::Lower) {
            kkt.template topRightCorner<n, m>() = qp.A.transpose();
        }
        kkt.template bottomLeftCorner<m, n>() = qp.A;
        kkt.template bottomRightCorner<m, m>() = -1.0 * rho_inv_vec.asDiagonal();

        // TODO: quick hack
        kkt_mat_sparse = kkt_mat.sparseView();
    }

    void form_KKT_rhs(const qp_t &qp, kkt_vec_t& rhs)
    {
        rhs.template head<n>() = _settings.sigma * x - qp.q;
        rhs.template tail<m>() = z - rho_inv_vec.cwiseProduct(y);
    }

    void box_projection(constraint_t& z, const constraint_t& l, const constraint_t& u)
    {
        z = z.cwiseMax(l).cwiseMin(u);
    }

    void constr_type_init(const qp_t &qp)
    {
        for (int i = 0; i < qp.l.RowsAtCompileTime; i++) {
            if (qp.l[i] < -LOOSE_BOUNDS_THRESH && qp.u[i] > LOOSE_BOUNDS_THRESH) {
                constr_type[i] = LOOSE_BOUNDS;
            } else if (qp.u[i] - qp.l[i] < RHO_TOL) {
                constr_type[i] = EQUALITY_CONSTRAINT;
            } else {
                constr_type[i] = INEQUALITY_CONSTRAINT;
            }
        }
    }

    void rho_update(Scalar rho0)
    {
        for (int i = 0; i < rho_vec.RowsAtCompileTime; i++) {
            switch (constr_type[i]) {
            case LOOSE_BOUNDS:
                rho_vec[i] = RHO_MIN;
                break;
            case EQUALITY_CONSTRAINT:
                rho_vec[i] = RHO_EQ_FACTOR*rho0;
                break;
            case INEQUALITY_CONSTRAINT: /* fall through */
            default:
                rho_vec[i] = rho0;
            };
        }
        rho_inv_vec = rho_vec.cwiseInverse();
        rho = rho0;
    }

    void update_state(const qp_t& qp)
    {
        Scalar norm_Ax, norm_z;
        norm_Ax = (qp.A*x).template lpNorm<Eigen::Infinity>();
        norm_z = z.template lpNorm<Eigen::Infinity>();
        _max_Ax_z_norm = fmax(norm_Ax, norm_z);

        Scalar norm_Px, norm_ATy, norm_q;
        norm_Px = (qp.P*x).template lpNorm<Eigen::Infinity>();
        norm_ATy = (qp.A.transpose()*y).template lpNorm<Eigen::Infinity>();
        norm_q = qp.q.template lpNorm<Eigen::Infinity>();
        _max_Px_ATy_q_norm = fmax(norm_Px, fmax(norm_ATy, norm_q));

        _info.res_prim = residual_prim(qp);
        _info.res_dual = residual_dual(qp);
    }

    Scalar rho_estimate(const Scalar rho0, const qp_t &qp) const
    {
        Scalar rp_norm, rd_norm;
        rp_norm = _info.res_prim / (_max_Ax_z_norm + DIV_BY_ZERO_REGUL);
        rd_norm = _info.res_dual / (_max_Px_ATy_q_norm + DIV_BY_ZERO_REGUL);

        Scalar rho_new = rho0 * sqrt(rp_norm/(rd_norm + DIV_BY_ZERO_REGUL));
        return rho_new;
    }

    Scalar eps_prim(const qp_t &qp) const
    {
        Scalar norm_Ax, norm_z;
        norm_Ax = (qp.A*x).template lpNorm<Eigen::Infinity>();
        norm_z = z.template lpNorm<Eigen::Infinity>();
        return _settings.eps_abs + _settings.eps_rel * fmax(norm_Ax, norm_z);
    }

    Scalar eps_dual(const qp_t &qp) const
    {
        Scalar norm_Px, norm_ATy, norm_q;
        norm_Px = (qp.P*x).template lpNorm<Eigen::Infinity>();
        norm_ATy = (qp.A.transpose()*y).template lpNorm<Eigen::Infinity>();
        norm_q = qp.q.template lpNorm<Eigen::Infinity>();
        return _settings.eps_abs + _settings.eps_rel * fmax(norm_Px, fmax(norm_ATy, norm_q));
    }

    Scalar residual_prim(const qp_t &qp) const
    {
        return (qp.A*x - z).template lpNorm<Eigen::Infinity>();
    }

    Scalar residual_dual(const qp_t &qp) const
    {
        return (qp.P*x + qp.q + qp.A.transpose()*y).template lpNorm<Eigen::Infinity>();
    }

    bool termination_criteria(const qp_t &qp)
    {
        // check residual norms to detect optimality
        if (_info.res_prim <= eps_prim(qp) && _info.res_dual <= eps_dual(qp)) {
            return true;
        }

        return false;
    }

#ifdef OSQP_PRINTING
    void print_status(const qp_t &qp) const
    {
        Scalar obj = 0.5 * x.dot(qp.P*x) + qp.q.dot(x);

        if (iter == 1) {
            printf("iter   obj       rp        rd\n");
        }
        printf("%4d  %.2e  %.2e  %.2e\n", iter, obj, _info.res_prim, _info.res_dual);
    }

    void print_settings(const settings_t &settings) const
    {
        printf("ADMM settings:\n");
        printf("  sigma %.2e\n", _settings.sigma);
        printf("  rho %.2e\n", _settings.rho);
        printf("  alpha %.2f\n", _settings.alpha);
        printf("  eps_rel %.1e\n", _settings.eps_rel);
        printf("  eps_abs %.1e\n", _settings.eps_abs);
        printf("  max_iter %d\n", _settings.max_iter);
        printf("  adaptive_rho %d\n", _settings.adaptive_rho);
        printf("  warm_start %d\n", _settings.warm_start);
    }
#endif
};

} // namespace qp_solver_sparse

#endif // QP_SOLVER_SPARSE_H
