/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file   SmartFactorBase.h
 * @brief  Base class to create smart factors on poses or cameras
 * @author Luca Carlone
 * @author Zsolt Kira
 * @author Frank Dellaert
 */

#pragma once

#include "JacobianFactorQ.h"
#include "ImplicitSchurFactor.h"
#include "RegularHessianFactor.h"

#include <gtsam/nonlinear/NonlinearFactor.h>
#include <gtsam/geometry/PinholeCamera.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/slam/dataset.h>

#include <boost/optional.hpp>
#include <boost/make_shared.hpp>
#include <vector>

namespace gtsam {
/// Base class with no internal point, completely functional
template<class POSE, class CALIBRATION, size_t D>
class SmartFactorBase: public NonlinearFactor {
protected:

  // Keep a copy of measurement and calibration for I/O
  std::vector<Point2> measured_; ///< 2D measurement for each of the m views
  std::vector<SharedNoiseModel> noise_; ///< noise model used
  ///< (important that the order is the same as the keys that we use to create the factor)

  boost::optional<POSE> body_P_sensor_; ///< The pose of the sensor in the body frame (one for all cameras)

  /// Definitions for blocks of F
  typedef Eigen::Matrix<double, 2, D> Matrix2D; // F
  typedef Eigen::Matrix<double, D, 2> MatrixD2; // F'
  typedef std::pair<Key, Matrix2D> KeyMatrix2D; // Fblocks
  typedef Eigen::Matrix<double, D, D> MatrixDD; // camera hessian block
  typedef Eigen::Matrix<double, D, 1> VectorD;
  typedef Eigen::Matrix<double, 2, 2> Matrix2;

  /// shorthand for base class type
  typedef NonlinearFactor Base;

  /// shorthand for this class
  typedef SmartFactorBase<POSE, CALIBRATION, D> This;

public:

  /// shorthand for a smart pointer to a factor
  typedef boost::shared_ptr<This> shared_ptr;

  /// shorthand for a pinhole camera
  typedef PinholeCamera<CALIBRATION> Camera;
  typedef std::vector<Camera> Cameras;

  /**
   * Constructor
   * @param body_P_sensor is the transform from body to sensor frame (default identity)
   */
  SmartFactorBase(boost::optional<POSE> body_P_sensor = boost::none) :
      body_P_sensor_(body_P_sensor) {
  }

  /** Virtual destructor */
  virtual ~SmartFactorBase() {
  }

  /**
   * add a new measurement and pose key
   * @param measured_i is the 2m dimensional projection of a single landmark
   * @param poseKey is the index corresponding to the camera observing the landmark
   * @param noise_i is the measurement noise
   */
  void add(const Point2& measured_i, const Key& poseKey_i,
      const SharedNoiseModel& noise_i) {
    this->measured_.push_back(measured_i);
    this->keys_.push_back(poseKey_i);
    this->noise_.push_back(noise_i);
  }

  /**
   * variant of the previous add: adds a bunch of measurements, together with the camera keys and noises
   */
  // ****************************************************************************************************
  void add(std::vector<Point2>& measurements, std::vector<Key>& poseKeys,
      std::vector<SharedNoiseModel>& noises) {
    for (size_t i = 0; i < measurements.size(); i++) {
      this->measured_.push_back(measurements.at(i));
      this->keys_.push_back(poseKeys.at(i));
      this->noise_.push_back(noises.at(i));
    }
  }

  /**
   * variant of the previous add: adds a bunch of measurements and uses the same noise model for all of them
   */
  // ****************************************************************************************************
  void add(std::vector<Point2>& measurements, std::vector<Key>& poseKeys,
      const SharedNoiseModel& noise) {
    for (size_t i = 0; i < measurements.size(); i++) {
      this->measured_.push_back(measurements.at(i));
      this->keys_.push_back(poseKeys.at(i));
      this->noise_.push_back(noise);
    }
  }

  /**
   * Adds an entire SfM_track (collection of cameras observing a single point).
   * The noise is assumed to be the same for all measurements
   */
  // ****************************************************************************************************
  void add(const SfM_Track& trackToAdd, const SharedNoiseModel& noise) {
    for (size_t k = 0; k < trackToAdd.number_measurements(); k++) {
      this->measured_.push_back(trackToAdd.measurements[k].second);
      this->keys_.push_back(trackToAdd.measurements[k].first);
      this->noise_.push_back(noise);
    }
  }

  /** return the measurements */
  const Vector& measured() const {
    return measured_;
  }

  /** return the noise model */
  const SharedNoiseModel& noise() const {
    return noise_;
  }

  /**
   * print
   * @param s optional string naming the factor
   * @param keyFormatter optional formatter useful for printing Symbols
   */
  void print(const std::string& s = "", const KeyFormatter& keyFormatter =
      DefaultKeyFormatter) const {
    std::cout << s << "SmartFactorBase, z = \n";
    for (size_t k = 0; k < measured_.size(); ++k) {
      std::cout << "measurement, p = " << measured_[k] << "\t";
      noise_[k]->print("noise model = ");
    }
    if (this->body_P_sensor_)
      this->body_P_sensor_->print("  sensor pose in body frame: ");
    Base::print("", keyFormatter);
  }

  /// equals
  virtual bool equals(const NonlinearFactor& p, double tol = 1e-9) const {
    const This *e = dynamic_cast<const This*>(&p);

    bool areMeasurementsEqual = true;
    for (size_t i = 0; i < measured_.size(); i++) {
      if (this->measured_.at(i).equals(e->measured_.at(i), tol) == false)
        areMeasurementsEqual = false;
      break;
    }
    return e && Base::equals(p, tol) && areMeasurementsEqual
        && ((!body_P_sensor_ && !e->body_P_sensor_)
            || (body_P_sensor_ && e->body_P_sensor_
                && body_P_sensor_->equals(*e->body_P_sensor_)));
  }

  // ****************************************************************************************************
  /// Calculate vector of re-projection errors, before applying noise model
  Vector reprojectionError(const Cameras& cameras, const Point3& point) const {

    Vector b = zero(2 * cameras.size());

    size_t i = 0;
    BOOST_FOREACH(const Camera& camera, cameras) {
      const Point2& zi = this->measured_.at(i);
      try {
        Point2 e(camera.project(point) - zi);
        b[2 * i] = e.x();
        b[2 * i + 1] = e.y();
      } catch (CheiralityException& e) {
        std::cout << "Cheirality exception " << std::endl;
        exit (EXIT_FAILURE);
      }
      i += 1;
    }

    return b;
  }

  // ****************************************************************************************************
  /**
   * Calculate the error of the factor.
   * This is the log-likelihood, e.g. \f$ 0.5(h(x)-z)^2/\sigma^2 \f$ in case of Gaussian.
   * In this class, we take the raw prediction error \f$ h(x)-z \f$, ask the noise model
   * to transform it to \f$ (h(x)-z)^2/\sigma^2 \f$, and then multiply by 0.5.
   * This is different from reprojectionError(cameras,point) as each point is whitened
   */
  double totalReprojectionError(const Cameras& cameras,
      const Point3& point) const {

    double overallError = 0;

    size_t i = 0;
    BOOST_FOREACH(const Camera& camera, cameras) {
      const Point2& zi = this->measured_.at(i);
      try {
        Point2 reprojectionError(camera.project(point) - zi);
        overallError += 0.5
        * this->noise_.at(i)->distance(reprojectionError.vector());
      } catch (CheiralityException& e) {
        std::cout << "Cheirality exception " << std::endl;
        exit (EXIT_FAILURE);
      }
      i += 1;
    }
    return overallError;
  }

  // ****************************************************************************************************
  /// Assumes non-degenerate !
  void computeEP(Matrix& E, Matrix& PointCov, const Cameras& cameras,
      const Point3& point) const {

    int numKeys = this->keys_.size();
    E = zeros(2 * numKeys, 3);
    Vector b = zero(2 * numKeys);

    Matrix Ei(2, 3);
    for (size_t i = 0; i < this->measured_.size(); i++) {
      try {
        cameras[i].project(point, boost::none, Ei);
      } catch (CheiralityException& e) {
        std::cout << "Cheirality exception " << std::endl;
        exit (EXIT_FAILURE);
      }
      this->noise_.at(i)->WhitenSystem(Ei, b);
      E.block<2, 3>(2 * i, 0) = Ei;
    }

    // Matrix PointCov;
    PointCov.noalias() = (E.transpose() * E).inverse();
  }

  // ****************************************************************************************************
  /// Compute F, E only (called below in both vanilla and SVD versions)
  /// Given a Point3, assumes dimensionality is 3
  double computeJacobians(std::vector<KeyMatrix2D>& Fblocks, Matrix& E,
      Vector& b, const Cameras& cameras, const Point3& point) const {

    int numKeys = this->keys_.size();
    E = zeros(2 * numKeys, 3);
    b = zero(2 * numKeys);
    double f = 0;

    Matrix Fi(2, 6), Ei(2, 3), Hcali(2, D - 6), Hcam(2, D);
    for (size_t i = 0; i < this->measured_.size(); i++) {

      Vector bi;
      try {
        bi =
            -(cameras[i].project(point, Fi, Ei, Hcali) - this->measured_.at(i)).vector();
      } catch (CheiralityException& e) {
        std::cout << "Cheirality exception " << std::endl;
        exit (EXIT_FAILURE);
      }
      this->noise_.at(i)->WhitenSystem(Fi, Ei, Hcali, bi);

      f += bi.squaredNorm();
      if (D == 6) { // optimize only camera pose
        Fblocks.push_back(KeyMatrix2D(this->keys_[i], Fi));
      } else {
        Hcam.block<2, 6>(0, 0) = Fi; // 2 x 6 block for the cameras
        Hcam.block<2, D - 6>(0, 6) = Hcali; // 2 x nrCal block for the cameras
        Fblocks.push_back(KeyMatrix2D(this->keys_[i], Hcam));
      }
      E.block<2, 3>(2 * i, 0) = Ei;
      subInsert(b, bi, 2 * i);
    }
    return f;
  }

  // ****************************************************************************************************
  /// Version that computes PointCov, with optional lambda parameter
  double computeJacobians(std::vector<KeyMatrix2D>& Fblocks, Matrix& E,
      Matrix3& PointCov, Vector& b, const Cameras& cameras, const Point3& point,
      double lambda = 0.0, bool diagonalDamping = false) const {

    double f = computeJacobians(Fblocks, E, b, cameras, point);

    // Point covariance inv(E'*E)
    Matrix3 EtE = E.transpose() * E;
    Matrix3 DMatrix = eye(E.cols()); // damping matrix

    if (diagonalDamping) { // diagonal of the hessian
      DMatrix(0, 0) = EtE(0, 0);
      DMatrix(1, 1) = EtE(1, 1);
      DMatrix(2, 2) = EtE(2, 2);
    }

    PointCov.noalias() = (EtE + lambda * DMatrix).inverse();

    return f;
  }

  // ****************************************************************************************************
  // TODO, there should not be a Matrix version, really
  double computeJacobians(Matrix& F, Matrix& E, Matrix3& PointCov, Vector& b,
      const Cameras& cameras, const Point3& point,
      const double lambda = 0.0) const {

    int numKeys = this->keys_.size();
    std::vector<KeyMatrix2D> Fblocks;
    double f = computeJacobians(Fblocks, E, PointCov, b, cameras, point,
        lambda);
    F = zeros(2 * numKeys, D * numKeys);

    for (size_t i = 0; i < this->keys_.size(); ++i) {
      F.block<2, D>(2 * i, D * i) = Fblocks.at(i).second; // 2 x 6 block for the cameras
    }
    return f;
  }

  // ****************************************************************************************************
  /// SVD version
  double computeJacobiansSVD(std::vector<KeyMatrix2D>& Fblocks, Matrix& Enull,
      Vector& b, const Cameras& cameras, const Point3& point,
      double lambda = 0.0, bool diagonalDamping = false) const {

    Matrix E;
    Matrix3 PointCov; // useless
    double f = computeJacobians(Fblocks, E, PointCov, b, cameras, point, lambda, diagonalDamping); // diagonalDamping should have no effect (only on PointCov)

    // Do SVD on A
    Eigen::JacobiSVD < Matrix > svd(E, Eigen::ComputeFullU);
    Vector s = svd.singularValues();
    // Enull = zeros(2 * numKeys, 2 * numKeys - 3);
    int numKeys = this->keys_.size();
    Enull = svd.matrixU().block(0, 3, 2 * numKeys, 2 * numKeys - 3); // last 2m-3 columns

    return f;
  }

  // ****************************************************************************************************
  /// Matrix version of SVD
  // TODO, there should not be a Matrix version, really
  double computeJacobiansSVD(Matrix& F, Matrix& Enull, Vector& b,
      const Cameras& cameras, const Point3& point) const {

    int numKeys = this->keys_.size();
    std::vector<KeyMatrix2D> Fblocks;
    double f = computeJacobiansSVD(Fblocks, Enull, b, cameras, point);
    F.resize(2 * numKeys, D * numKeys);
    F.setZero();

    for (size_t i = 0; i < this->keys_.size(); ++i)
      F.block<2, D>(2 * i, D * i) = Fblocks.at(i).second; // 2 x 6 block for the cameras

    return f;
  }

  // ****************************************************************************************************
  /// linearize returns a Hessianfactor that is an approximation of error(p)
  boost::shared_ptr<RegularHessianFactor<D> > createHessianFactor(
      const Cameras& cameras, const Point3& point, const double lambda = 0.0,
      bool diagonalDamping = false) const {

    int numKeys = this->keys_.size();

    std::vector<KeyMatrix2D> Fblocks;
    Matrix E;
    Matrix3 PointCov;
    Vector b;
    double f = computeJacobians(Fblocks, E, PointCov, b, cameras, point, lambda,
        diagonalDamping);

    // Create structures for Hessian Factors
    std::vector < Matrix > Gs(numKeys * (numKeys + 1) / 2);
    std::vector < Vector > gs(numKeys);

    sparseSchurComplement(Fblocks, E, PointCov, b, Gs, gs);
    // schurComplement(Fblocks, E, PointCov, b, Gs, gs);

    //std::vector < Matrix > Gs2(Gs.begin(), Gs.end());
    //std::vector < Vector > gs2(gs.begin(), gs.end());

    return boost::make_shared < RegularHessianFactor<D>
        > (this->keys_, Gs, gs, f);
  }

  // ****************************************************************************************************
  void schurComplement(const std::vector<KeyMatrix2D>& Fblocks, const Matrix& E,
      const Matrix& PointCov, const Vector& b,
      /*output ->*/std::vector<Matrix>& Gs, std::vector<Vector>& gs) const {
    // Schur complement trick
    // Gs = F' * F - F' * E * inv(E'*E) * E' * F
    // gs = F' * (b - E * inv(E'*E) * E' * b)
    // This version uses full matrices

    int numKeys = this->keys_.size();

    /// Compute Full F ????
    Matrix F = zeros(2 * numKeys, D * numKeys);
    for (size_t i = 0; i < this->keys_.size(); ++i)
      F.block<2, D>(2 * i, D * i) = Fblocks.at(i).second; // 2 x 6 block for the cameras

    Matrix H(D * numKeys, D * numKeys);
    Vector gs_vector;

    H.noalias() = F.transpose() * (F - (E * (PointCov * (E.transpose() * F))));
    gs_vector.noalias() = F.transpose()
        * (b - (E * (PointCov * (E.transpose() * b))));

    // Populate Gs and gs
    int GsCount2 = 0;
    for (DenseIndex i1 = 0; i1 < numKeys; i1++) { // for each camera
      DenseIndex i1D = i1 * D;
      gs.at(i1) = gs_vector.segment < D > (i1D);
      for (DenseIndex i2 = 0; i2 < numKeys; i2++) {
        if (i2 >= i1) {
          Gs.at(GsCount2) = H.block<D, D>(i1D, i2 * D);
          GsCount2++;
        }
      }
    }
  }

  // ****************************************************************************************************
  void sparseSchurComplement(const std::vector<KeyMatrix2D>& Fblocks,
      const Matrix& E, const Matrix& PointCov, const Vector& b,
      /*output ->*/std::vector<Matrix>& Gs, std::vector<Vector>& gs) const {
    // Schur complement trick
    // Gs = F' * F - F' * E * inv(E'*E) * E' * F
    // gs = F' * (b - E * inv(E'*E) * E' * b)

    // a single point is observed in numKeys cameras
    size_t numKeys = this->keys_.size();

    // Blockwise Schur complement
    int GsCount = 0;
    for (size_t i1 = 0; i1 < numKeys; i1++) { // for each camera
      const Matrix2D& Fi1 = Fblocks.at(i1).second;
      // D = (Dx2) * (2)
      gs.at(i1) = Fi1.transpose() * b.segment < 2 > (2 * i1); // F' * b

      for (size_t i2 = 0; i2 < numKeys; i2++) {
        const Matrix2D& Fi2 = Fblocks.at(i2).second;

        // Compute (Ei1 * PointCov * Ei2')
        // (2x2) = (2x3) * (3x3) * (3x2)
        Matrix2 E_invEtE_Et = E.block<2, 3>(2 * i1, 0) * PointCov * (E.block<2, 3>(2 * i2, 0)).transpose();

        // D = (Dx2) * (2x2) * (2)
        gs.at(i1) -= Fi1.transpose() *  ( E_invEtE_Et * b.segment < 2 > (2 * i2) );

        if (i2 == i1) { // diagonal entries
          // (DxD) = (Dx2) * ( (2xD) - (2x2) * (2xD) )
          Gs.at(GsCount) = Fi1.transpose() * (Fi1 -  E_invEtE_Et * Fi2);
          GsCount++;
        }
        if (i2 > i1) { // off diagonal
          // (DxD) = (Dx2) * ( (2x2) * (2xD) )
          Gs.at(GsCount) = - Fi1.transpose() *  ( E_invEtE_Et * Fi2 );
          GsCount++;
        }
      }
    }
  }

  // ****************************************************************************************************
  boost::shared_ptr<ImplicitSchurFactor<D> > createImplicitSchurFactor(
      const Cameras& cameras, const Point3& point, double lambda = 0.0,
      bool diagonalDamping = false) const {
    typename boost::shared_ptr<ImplicitSchurFactor<D> > f(
        new ImplicitSchurFactor<D>());
    computeJacobians(f->Fblocks(), f->E(), f->PointCovariance(), f->b(),
        cameras, point, lambda, diagonalDamping);
    f->initKeys();
    return f;
  }

  // ****************************************************************************************************
  boost::shared_ptr<JacobianFactorQ<D> > createJacobianQFactor(
      const Cameras& cameras, const Point3& point, double lambda = 0.0,
      bool diagonalDamping = false) const {
    std::vector<KeyMatrix2D> Fblocks;
    Matrix E;
    Matrix3 PointCov;
    Vector b;
    computeJacobians(Fblocks, E, PointCov, b, cameras, point, lambda,
        diagonalDamping);
    return boost::make_shared < JacobianFactorQ<D> > (Fblocks, E, PointCov, b);
  }

private:

  /// Serialization function
  friend class boost::serialization::access;
  template<class ARCHIVE>
  void serialize(ARCHIVE & ar, const unsigned int version) {
    ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(Base);
    ar & BOOST_SERIALIZATION_NVP(measured_);
    ar & BOOST_SERIALIZATION_NVP(body_P_sensor_);
  }
};

} // \ namespace gtsam
