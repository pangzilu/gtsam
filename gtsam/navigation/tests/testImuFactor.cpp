/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    testImuFactor.cpp
 * @brief   Unit test for ImuFactor
 * @author  Luca Carlone, Stephen Williams, Richard Roberts
 */

#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/nonlinear/factorTesting.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/base/TestableAssertions.h>
#include <gtsam/base/numericalDerivative.h>
#include <CppUnitLite/TestHarness.h>

#include <boost/bind.hpp>
#include <list>

using namespace std;
using namespace gtsam;

// Convenience for named keys
using symbol_shorthand::X;
using symbol_shorthand::V;
using symbol_shorthand::B;

static const Vector3 kGravityAlongNavZDown(0, 0, 9.81);
static const Vector3 kZeroOmegaCoriolis(0, 0, 0);
static const Vector3 kNonZeroOmegaCoriolis(0, 0.1, 0.1);

/* ************************************************************************* */
namespace {
// Auxiliary functions to test evaluate error in ImuFactor
/* ************************************************************************* */
Rot3 evaluateRotationError(const ImuFactor& factor, const Pose3& pose_i,
    const Vector3& vel_i, const Pose3& pose_j, const Vector3& vel_j,
    const imuBias::ConstantBias& bias) {
  return Rot3::Expmap(
      factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias).head(3));
}

// Auxiliary functions to test Jacobians F and G used for
// covariance propagation during preintegration
/* ************************************************************************* */
Vector6 updatePreintegratedPosVel(const Vector3 deltaPij_old,
    const Vector3& deltaVij_old, const Rot3& deltaRij_old,
    const Vector3& correctedAcc, const Vector3& correctedOmega,
    const double deltaT) {
  Matrix3 dRij = deltaRij_old.matrix();
  Vector3 temp = dRij * correctedAcc * deltaT;
  Vector3 deltaPij_new;
  deltaPij_new = deltaPij_old + deltaVij_old * deltaT + 0.5 * temp * deltaT;
  Vector3 deltaVij_new = deltaVij_old + temp;

  Vector6 result;
  result << deltaPij_new, deltaVij_new;
  return result;
}

Rot3 updatePreintegratedRot(const Rot3& deltaRij_old,
    const Vector3& correctedOmega, const double deltaT) {
  Rot3 deltaRij_new = deltaRij_old * Rot3::Expmap(correctedOmega * deltaT);
  return deltaRij_new;
}

// Define covariance matrices
/* ************************************************************************* */
double accNoiseVar = 0.01;
double omegaNoiseVar = 0.03;
double intNoiseVar = 0.0001;
const Matrix3 kMeasuredAccCovariance = accNoiseVar * I_3x3;
const Matrix3 kMeasuredOmegaCovariance = omegaNoiseVar * I_3x3;
const Matrix3 kIntegrationErrorCovariance = intNoiseVar * I_3x3;

// Auxiliary functions to test preintegrated Jacobians
// delPdelBiasAcc_ delPdelBiasOmega_ delVdelBiasAcc_ delVdelBiasOmega_ delRdelBiasOmega_
/* ************************************************************************* */
PreintegratedImuMeasurements evaluatePreintegratedMeasurements(
    const imuBias::ConstantBias& bias, const list<Vector3>& measuredAccs,
    const list<Vector3>& measuredOmegas, const list<double>& deltaTs) {
  PreintegratedImuMeasurements result(bias, kMeasuredAccCovariance,
      kMeasuredOmegaCovariance, kIntegrationErrorCovariance);

  list<Vector3>::const_iterator itAcc = measuredAccs.begin();
  list<Vector3>::const_iterator itOmega = measuredOmegas.begin();
  list<double>::const_iterator itDeltaT = deltaTs.begin();
  for (; itAcc != measuredAccs.end(); ++itAcc, ++itOmega, ++itDeltaT) {
    result.integrateMeasurement(*itAcc, *itOmega, *itDeltaT);
  }
  return result;
}

Vector3 evaluatePreintegratedMeasurementsPosition(
    const imuBias::ConstantBias& bias, const list<Vector3>& measuredAccs,
    const list<Vector3>& measuredOmegas, const list<double>& deltaTs) {
  return evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
      deltaTs).deltaPij();
}

Vector3 evaluatePreintegratedMeasurementsVelocity(
    const imuBias::ConstantBias& bias, const list<Vector3>& measuredAccs,
    const list<Vector3>& measuredOmegas, const list<double>& deltaTs) {
  return evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
      deltaTs).deltaVij();
}

Rot3 evaluatePreintegratedMeasurementsRotation(
    const imuBias::ConstantBias& bias, const list<Vector3>& measuredAccs,
    const list<Vector3>& measuredOmegas, const list<double>& deltaTs) {
  return Rot3(
      evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
          deltaTs).deltaRij());
}

Rot3 evaluateRotation(const Vector3 measuredOmega, const Vector3 biasOmega,
    const double deltaT) {
  return Rot3::Expmap((measuredOmega - biasOmega) * deltaT);
}

Vector3 evaluateLogRotation(const Vector3 thetahat, const Vector3 deltatheta) {
  return Rot3::Logmap(Rot3::Expmap(thetahat).compose(Rot3::Expmap(deltatheta)));
}

} // namespace

/* ************************************************************************* */
TEST(ImuFactor, StraightLine) {
  // Set up IMU measurements
  static const double g = 10; // make this easy to check
  static const double a = 0.2, dt = 3.0;
  const double exact = dt * dt / 2;
  Vector3 measuredAcc(a, 0, -g), measuredOmega(0, 0, 0);

  // Set up body pointing towards y axis, and start at 10,20,0 with zero velocity
  // TODO(frank): clean up Rot3 mess
  static const Rot3 nRb(Point3(0, 1, 0), Point3(1, 0, 0), Point3(0, 0, -1));
  static const Point3 initial_position(10, 20, 0);
  static const NavState state1(nRb, initial_position, Velocity3(0, 0, 0));

  imuBias::ConstantBias biasHat, bias;
  boost::shared_ptr<PreintegratedImuMeasurements::Params> p =
      PreintegratedImuMeasurements::Params::MakeSharedU(g); // Up convention!

  PreintegratedImuMeasurements pim(p, biasHat);
  for (size_t i = 0; i < 10; i++)
    pim.integrateMeasurement(measuredAcc, measuredOmega, dt / 10);

  // Check integrated quantities in body frame: gravity measured by IMU is integrated!
  EXPECT(assert_equal(Rot3(), pim.deltaRij()));
  EXPECT(assert_equal(Vector3(a * exact, 0, -g * exact), pim.deltaPij()));
  EXPECT(assert_equal(Vector3(a * dt, 0, -g * dt), pim.deltaVij()));

  // Check bias-corrected delta: also in body frame
  Vector9 expectedBC;
  expectedBC << 0, 0, 0, a * exact, 0, -g * exact, a * dt, 0, -g * dt;
  EXPECT(assert_equal(expectedBC, pim.biasCorrectedDelta(bias)));

  // Check prediction, note we move along x in body, along y in nav
  NavState expected(nRb, Point3(10, 20 + a * exact, 0),
      Velocity3(0, a * dt, 0));
  EXPECT(assert_equal(expected, pim.predict(state1, bias)));
}

/* ************************************************************************* */
TEST(ImuFactor, PreintegratedMeasurements) {
  // Linearization point
  imuBias::ConstantBias bias(Vector3(0, 0, 0), Vector3(0, 0, 0));

  // Measurements
  Vector3 measuredAcc(0.1, 0.0, 0.0);
  Vector3 measuredOmega(M_PI / 100.0, 0.0, 0.0);
  double deltaT = 0.5;

  // Expected preintegrated values
  Vector3 expectedDeltaP1;
  expectedDeltaP1 << 0.5 * 0.1 * 0.5 * 0.5, 0, 0;
  Vector3 expectedDeltaV1(0.05, 0.0, 0.0);
  Rot3 expectedDeltaR1 = Rot3::RzRyRx(0.5 * M_PI / 100.0, 0.0, 0.0);
  double expectedDeltaT1(0.5);

  // Actual preintegrated values
  PreintegratedImuMeasurements actual1(bias, kMeasuredAccCovariance,
      kMeasuredOmegaCovariance, kIntegrationErrorCovariance);
  actual1.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  EXPECT(assert_equal(Vector(expectedDeltaP1), Vector(actual1.deltaPij())));
  EXPECT(assert_equal(Vector(expectedDeltaV1), Vector(actual1.deltaVij())));
  EXPECT(assert_equal(expectedDeltaR1, Rot3(actual1.deltaRij())));
  DOUBLES_EQUAL(expectedDeltaT1, actual1.deltaTij(), 1e-9);

  // Integrate again
  Vector3 expectedDeltaP2;
  expectedDeltaP2 << 0.025 + expectedDeltaP1(0) + 0.5 * 0.1 * 0.5 * 0.5, 0, 0;
  Vector3 expectedDeltaV2 = Vector3(0.05, 0.0, 0.0)
      + expectedDeltaR1.matrix() * measuredAcc * 0.5;
  Rot3 expectedDeltaR2 = Rot3::RzRyRx(2.0 * 0.5 * M_PI / 100.0, 0.0, 0.0);
  double expectedDeltaT2(1);

  // Actual preintegrated values
  PreintegratedImuMeasurements actual2 = actual1;
  actual2.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  EXPECT(assert_equal(Vector(expectedDeltaP2), Vector(actual2.deltaPij())));
  EXPECT(assert_equal(Vector(expectedDeltaV2), Vector(actual2.deltaVij())));
  EXPECT(assert_equal(expectedDeltaR2, Rot3(actual2.deltaRij())));
  DOUBLES_EQUAL(expectedDeltaT2, actual2.deltaTij(), 1e-9);
}

/* ************************************************************************* */
// Common linearization point and measurements for tests
namespace common {
static const imuBias::ConstantBias biasHat, bias; // Bias
static const Pose3 x1(Rot3::RzRyRx(M_PI / 12.0, M_PI / 6.0, M_PI / 4.0),
    Point3(5.0, 1.0, 0));
static const Vector3 v1(Vector3(0.5, 0.0, 0.0));
static const NavState state1(x1, v1);

// Measurements
static const double w = M_PI / 100;
static const Vector3 measuredOmega(w, 0, 0);
static const Vector3 measuredAcc = x1.rotation().unrotate(
    -kGravityAlongNavZDown);
static const double deltaT = 1.0;

static const Pose3 x2(Rot3::RzRyRx(M_PI / 12.0 + w, M_PI / 6.0, M_PI / 4.0),
    Point3(5.5, 1.0, 0));
static const Vector3 v2(Vector3(0.5, 0.0, 0.0));
static const NavState state2(x2, v2);
} // namespace common

/* ************************************************************************* */
TEST(ImuFactor, PreintegrationBaseMethods) {
  using namespace common;
  boost::shared_ptr<PreintegratedImuMeasurements::Params> p =
      PreintegratedImuMeasurements::Params::MakeSharedD();
  p->gyroscopeCovariance = kMeasuredOmegaCovariance;
  p->omegaCoriolis = Vector3(0.02, 0.03, 0.04);
  p->accelerometerCovariance = kMeasuredAccCovariance;
  p->integrationCovariance = kIntegrationErrorCovariance;
  p->use2ndOrderCoriolis = true;

  PreintegratedImuMeasurements pim(p, bias);
  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);
  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  // biasCorrectedDelta
  Matrix96 actualH;
  pim.biasCorrectedDelta(bias, actualH);
  Matrix expectedH = numericalDerivative11<Vector9, imuBias::ConstantBias>(
      boost::bind(&PreintegrationBase::biasCorrectedDelta, pim, _1,
          boost::none), bias);
  EXPECT(assert_equal(expectedH, actualH));

  Matrix9 aH1;
  Matrix96 aH2;
  NavState predictedState = pim.predict(state1, bias, aH1, aH2);
  Matrix eH1 = numericalDerivative11<NavState, NavState>(
      boost::bind(&PreintegrationBase::predict, pim, _1, bias, boost::none,
          boost::none), state1);
  EXPECT(assert_equal(eH1, aH1));
  Matrix eH2 = numericalDerivative11<NavState, imuBias::ConstantBias>(
      boost::bind(&PreintegrationBase::predict, pim, state1, _1, boost::none,
          boost::none), bias);
  EXPECT(assert_equal(eH2, aH2));
  return;

}

/* ************************************************************************* */
TEST(ImuFactor, ErrorAndJacobians) {
  using namespace common;
  PreintegratedImuMeasurements pim(biasHat, kMeasuredAccCovariance,
      kMeasuredOmegaCovariance, kIntegrationErrorCovariance);

  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);
  EXPECT(assert_equal(state2, pim.predict(state1, bias)));

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);

  // Expected error
  Vector expectedError(9);
  expectedError << 0, 0, 0, 0, 0, 0, 0, 0, 0;
  EXPECT(
      assert_equal(expectedError, factor.evaluateError(x1, v1, x2, v2, bias)));

  Values values;
  values.insert(X(1), x1);
  values.insert(V(1), v1);
  values.insert(X(2), x2);
  values.insert(V(2), v2);
  values.insert(B(1), bias);
  EXPECT(assert_equal(expectedError, factor.unwhitenedError(values)));

  // Make sure linearization is correct
  double diffDelta = 1e-7;
  EXPECT_CORRECT_FACTOR_JACOBIANS(factor, values, diffDelta, 1e-3);

  // Actual Jacobians
  Matrix H1a, H2a, H3a, H4a, H5a;
  (void) factor.evaluateError(x1, v1, x2, v2, bias, H1a, H2a, H3a, H4a, H5a);

  // Make sure rotation part is correct when error is interpreted as axis-angle
  // Jacobians are around zero, so the rotation part is the same as:
  Matrix H1Rot3 = numericalDerivative11<Rot3, Pose3>(
      boost::bind(&evaluateRotationError, factor, _1, v1, x2, v2, bias), x1);
  EXPECT(assert_equal(H1Rot3, H1a.topRows(3)));

  Matrix H3Rot3 = numericalDerivative11<Rot3, Pose3>(
      boost::bind(&evaluateRotationError, factor, x1, v1, _1, v2, bias), x2);
  EXPECT(assert_equal(H3Rot3, H3a.topRows(3)));

  // Evaluate error with wrong values
  Vector3 v2_wrong = v2 + Vector3(0.1, 0.1, 0.1);
  values.update(V(2), v2_wrong);
  expectedError << 0, 0, 0, 0, 0, 0, -0.0724744871, -0.040715657, -0.151952901;
  EXPECT(
      assert_equal(expectedError,
          factor.evaluateError(x1, v1, x2, v2_wrong, bias), 1e-2));
  EXPECT(assert_equal(expectedError, factor.unwhitenedError(values), 1e-2));

  // Make sure the whitening is done correctly
  Matrix cov = pim.preintMeasCov();
  Matrix R = RtR(cov.inverse());
  Vector whitened = R * expectedError;
  EXPECT(assert_equal(0.5 * whitened.squaredNorm(), factor.error(values), 1e-5));

  // Make sure linearization is correct
  EXPECT_CORRECT_FACTOR_JACOBIANS(factor, values, diffDelta, 1e-3);
}

/* ************************************************************************* */
TEST(ImuFactor, ErrorAndJacobianWithBiases) {
  using common::x1;
  using common::v1;
  using common::v2;
  imuBias::ConstantBias bias(Vector3(0.2, 0, 0), Vector3(0.1, 0, 0.3)); // Biases (acc, rot)
  Pose3 x2(Rot3::Expmap(Vector3(0, 0, M_PI / 10.0 + M_PI / 10.0)),
      Point3(5.5, 1.0, -50.0));

  // Measurements
  Vector3 measuredOmega;
  measuredOmega << 0, 0, M_PI / 10.0 + 0.3;
  Vector3 measuredAcc = x1.rotation().unrotate(-kGravityAlongNavZDown)
      + Vector3(0.2, 0.0, 0.0);
  double deltaT = 1.0;

  imuBias::ConstantBias biasHat(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.1));
  PreintegratedImuMeasurements pim(biasHat, kMeasuredAccCovariance,
      kMeasuredOmegaCovariance, kIntegrationErrorCovariance);
  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  // Make sure of biascorrectedDeltaRij with this example...
  Matrix3 aH;
  pim.biascorrectedDeltaRij(bias.gyroscope(), aH);
  Matrix3 eH = numericalDerivative11<Rot3, Vector3>(
      boost::bind(&PreintegrationBase::biascorrectedDeltaRij, pim, _1,
          boost::none), bias.gyroscope());
  EXPECT(assert_equal(eH, aH));

  // ... and of biasCorrectedDelta
  Matrix96 actualH;
  pim.biasCorrectedDelta(bias, actualH);
  Matrix expectedH = numericalDerivative11<Vector9, imuBias::ConstantBias>(
      boost::bind(&PreintegrationBase::biasCorrectedDelta, pim, _1,
          boost::none), bias);
  EXPECT(assert_equal(expectedH, actualH));

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kNonZeroOmegaCoriolis);

  Values values;
  values.insert(X(1), x1);
  values.insert(V(1), v1);
  values.insert(X(2), x2);
  values.insert(V(2), v2);
  values.insert(B(1), bias);

  // Make sure linearization is correct
  double diffDelta = 1e-7;
  EXPECT_CORRECT_FACTOR_JACOBIANS(factor, values, diffDelta, 1e-3);
}

/* ************************************************************************* */
TEST(ImuFactor, ErrorAndJacobianWith2ndOrderCoriolis) {
  using common::x1;
  using common::v1;
  using common::v2;
  imuBias::ConstantBias bias(Vector3(0.2, 0, 0), Vector3(0.1, 0, 0.3)); // Biases (acc, rot)
  Pose3 x2(Rot3::Expmap(Vector3(0, 0, M_PI / 10.0 + M_PI / 10.0)),
      Point3(5.5, 1.0, -50.0));

  // Measurements
  Vector3 measuredOmega;
  measuredOmega << 0, 0, M_PI / 10.0 + 0.3;
  Vector3 measuredAcc = x1.rotation().unrotate(-kGravityAlongNavZDown)
      + Vector3(0.2, 0.0, 0.0);
  double deltaT = 1.0;

  PreintegratedImuMeasurements pim(
      imuBias::ConstantBias(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.1)),
      kMeasuredAccCovariance, kMeasuredOmegaCovariance,
      kIntegrationErrorCovariance);
  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  // Create factor
  Pose3 bodyPsensor = Pose3();
  bool use2ndOrderCoriolis = true;
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kNonZeroOmegaCoriolis, bodyPsensor, use2ndOrderCoriolis);

  Values values;
  values.insert(X(1), x1);
  values.insert(V(1), v1);
  values.insert(X(2), x2);
  values.insert(V(2), v2);
  values.insert(B(1), bias);

  // Make sure linearization is correct
  double diffDelta = 1e-7;
  EXPECT_CORRECT_FACTOR_JACOBIANS(factor, values, diffDelta, 1e-3);
}

/* ************************************************************************* */
TEST(ImuFactor, PartialDerivative_wrt_Bias) {
  // Linearization point
  Vector3 biasOmega(0, 0, 0); // Current estimate of rotation rate bias

  // Measurements
  Vector3 measuredOmega(0.1, 0, 0);
  double deltaT = 0.5;

  // Compute numerical derivatives
  Matrix expectedDelRdelBiasOmega = numericalDerivative11<Rot3, Vector3>(
      boost::bind(&evaluateRotation, measuredOmega, _1, deltaT),
      Vector3(biasOmega));

  const Matrix3 Jr = Rot3::ExpmapDerivative(
      (measuredOmega - biasOmega) * deltaT);

  Matrix3 actualdelRdelBiasOmega = -Jr * deltaT; // the delta bias appears with the minus sign

  // Compare Jacobians
  EXPECT(assert_equal(expectedDelRdelBiasOmega, actualdelRdelBiasOmega, 1e-9));
}

/* ************************************************************************* */
TEST(ImuFactor, PartialDerivativeLogmap) {
  // Linearization point
  Vector3 thetahat(0.1, 0.1, 0); // Current estimate of rotation rate bias

  // Measurements
  Vector3 deltatheta(0, 0, 0);

  // Compute numerical derivatives
  Matrix expectedDelFdeltheta = numericalDerivative11<Vector, Vector3>(
      boost::bind(&evaluateLogRotation, thetahat, _1), Vector3(deltatheta));

  Matrix3 actualDelFdeltheta = Rot3::LogmapDerivative(thetahat);

  // Compare Jacobians
  EXPECT(assert_equal(expectedDelFdeltheta, actualDelFdeltheta));
}

/* ************************************************************************* */
TEST(ImuFactor, fistOrderExponential) {
  // Linearization point
  Vector3 biasOmega(0, 0, 0); // Current estimate of rotation rate bias

  // Measurements
  Vector3 measuredOmega(0.1, 0, 0);
  double deltaT = 1.0;

  // change w.r.t. linearization point
  double alpha = 0.0;
  Vector3 deltabiasOmega;
  deltabiasOmega << alpha, alpha, alpha;

  const Matrix3 Jr = Rot3::ExpmapDerivative(
      (measuredOmega - biasOmega) * deltaT);

  Matrix3 delRdelBiasOmega = -Jr * deltaT; // the delta bias appears with the minus sign

  const Matrix expectedRot = Rot3::Expmap(
      (measuredOmega - biasOmega - deltabiasOmega) * deltaT).matrix();

  const Matrix3 hatRot =
      Rot3::Expmap((measuredOmega - biasOmega) * deltaT).matrix();
  const Matrix3 actualRot = hatRot
      * Rot3::Expmap(delRdelBiasOmega * deltabiasOmega).matrix();
  // hatRot * (I_3x3 + skewSymmetric(delRdelBiasOmega * deltabiasOmega));

  // This is a first order expansion so the equality is only an approximation
  EXPECT(assert_equal(expectedRot, actualRot));
}

/* ************************************************************************* */
TEST(ImuFactor, FirstOrderPreIntegratedMeasurements) {
  // Linearization point
  imuBias::ConstantBias bias; // Current estimate of acceleration and rotation rate biases

  Pose3 body_P_sensor(Rot3::Expmap(Vector3(0, 0.1, 0.1)), Point3(1, 0, 1));

  // Measurements
  list<Vector3> measuredAccs, measuredOmegas;
  list<double> deltaTs;
  measuredAccs.push_back(Vector3(0.1, 0.0, 0.0));
  measuredOmegas.push_back(Vector3(M_PI / 100.0, 0.0, 0.0));
  deltaTs.push_back(0.01);
  measuredAccs.push_back(Vector3(0.1, 0.0, 0.0));
  measuredOmegas.push_back(Vector3(M_PI / 100.0, 0.0, 0.0));
  deltaTs.push_back(0.01);
  for (int i = 1; i < 100; i++) {
    measuredAccs.push_back(Vector3(0.05, 0.09, 0.01));
    measuredOmegas.push_back(
        Vector3(M_PI / 100.0, M_PI / 300.0, 2 * M_PI / 100.0));
    deltaTs.push_back(0.01);
  }

  // Actual preintegrated values
  PreintegratedImuMeasurements preintegrated =
      evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
          deltaTs);

  // Compute numerical derivatives
  Matrix expectedDelPdelBias = numericalDerivative11<Vector,
      imuBias::ConstantBias>(
      boost::bind(&evaluatePreintegratedMeasurementsPosition, _1, measuredAccs,
          measuredOmegas, deltaTs), bias);
  Matrix expectedDelPdelBiasAcc = expectedDelPdelBias.leftCols(3);
  Matrix expectedDelPdelBiasOmega = expectedDelPdelBias.rightCols(3);

  Matrix expectedDelVdelBias = numericalDerivative11<Vector,
      imuBias::ConstantBias>(
      boost::bind(&evaluatePreintegratedMeasurementsVelocity, _1, measuredAccs,
          measuredOmegas, deltaTs), bias);
  Matrix expectedDelVdelBiasAcc = expectedDelVdelBias.leftCols(3);
  Matrix expectedDelVdelBiasOmega = expectedDelVdelBias.rightCols(3);

  Matrix expectedDelRdelBias =
      numericalDerivative11<Rot3, imuBias::ConstantBias>(
          boost::bind(&evaluatePreintegratedMeasurementsRotation, _1,
              measuredAccs, measuredOmegas, deltaTs), bias);
  Matrix expectedDelRdelBiasAcc = expectedDelRdelBias.leftCols(3);
  Matrix expectedDelRdelBiasOmega = expectedDelRdelBias.rightCols(3);

  // Compare Jacobians
  EXPECT(assert_equal(expectedDelPdelBiasAcc, preintegrated.delPdelBiasAcc()));
  EXPECT(
      assert_equal(expectedDelPdelBiasOmega, preintegrated.delPdelBiasOmega()));
  EXPECT(assert_equal(expectedDelVdelBiasAcc, preintegrated.delVdelBiasAcc()));
  EXPECT(
      assert_equal(expectedDelVdelBiasOmega, preintegrated.delVdelBiasOmega()));
  EXPECT(assert_equal(expectedDelRdelBiasAcc, Matrix::Zero(3, 3)));
  EXPECT(
      assert_equal(expectedDelRdelBiasOmega, preintegrated.delRdelBiasOmega()));
}

/* ************************************************************************* */
TEST(ImuFactor, JacobianPreintegratedCovariancePropagation) {
  // Linearization point
  imuBias::ConstantBias bias; // Current estimate of acceleration and rotation rate biases

  // Measurements
  const Vector3 measuredAcc(0.1, 0.0, 0.0);
  const Vector3 measuredOmega(M_PI / 100.0, 0.0, 0.0);
  const double deltaT = 0.01;
  list<Vector3> measuredAccs, measuredOmegas;
  list<double> deltaTs;
  measuredAccs.push_back(measuredAcc);
  measuredOmegas.push_back(measuredOmega);
  deltaTs.push_back(deltaT);
  measuredAccs.push_back(measuredAcc);
  measuredOmegas.push_back(measuredOmega);
  deltaTs.push_back(deltaT);
  for (int i = 1; i < 100; i++) {
    measuredAccs.push_back(Vector3(0.05, 0.09, 0.01));
    measuredOmegas.push_back(
        Vector3(M_PI / 100.0, M_PI / 300.0, 2 * M_PI / 100.0));
    deltaTs.push_back(deltaT);
  }
  // Actual preintegrated values
  PreintegratedImuMeasurements preintegrated =
      evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
          deltaTs);

  // so far we only created a nontrivial linearization point for the preintegrated measurements
  // Now we add a new measurement and ask for Jacobians
  const Rot3 deltaRij_old = preintegrated.deltaRij(); // before adding new measurement
  const Vector3 deltaVij_old = preintegrated.deltaVij(); // before adding new measurement
  const Vector3 deltaPij_old = preintegrated.deltaPij(); // before adding new measurement

  Matrix oldPreintCovariance = preintegrated.preintMeasCov();

  Matrix Factual, Gactual;
  preintegrated.integrateMeasurement(measuredAcc, measuredOmega, deltaT,
      Factual, Gactual);

  //////////////////////////////////////////////////////////////////////////////////////////////
  // COMPUTE NUMERICAL DERIVATIVES FOR F
  //////////////////////////////////////////////////////////////////////////////////////////////
  // Compute expected f_pos_vel wrt positions
  boost::function<Vector6(const Vector3&, const Vector3&, const Rot3&)> f =
      boost::bind(&updatePreintegratedPosVel, _1, _2, _3, measuredAcc,
          measuredOmega, deltaT);
  Matrix63 dfpv_dpos = numericalDerivative31(f, deltaPij_old, deltaVij_old,
      deltaRij_old);

  // Compute expected f_pos_vel wrt velocities
  Matrix63 dfpv_dvel = numericalDerivative32(f, deltaPij_old, deltaVij_old,
      deltaRij_old);

  // Compute expected f_pos_vel wrt angles
  Matrix63 dfpv_dangle = numericalDerivative33(f, deltaPij_old, deltaVij_old,
      deltaRij_old);

  // Compute expected f_rot wrt angles
  Matrix3 dfr_dangle = numericalDerivative11<Rot3, Rot3>(
      boost::bind(&updatePreintegratedRot, _1, measuredOmega, deltaT),
      deltaRij_old);

  Matrix Fexpected(9, 9);
  Fexpected << //
      dfr_dangle, Z_3x3, Z_3x3, //
  dfpv_dangle, dfpv_dpos, dfpv_dvel;

  EXPECT(assert_equal(Fexpected, Factual));

  //////////////////////////////////////////////////////////////////////////////////////////////
  // COMPUTE NUMERICAL DERIVATIVES FOR G
  //////////////////////////////////////////////////////////////////////////////////////////////
  // Compute jacobian wrt integration noise
  Matrix dgpv_dintNoise(6, 3);
  dgpv_dintNoise << I_3x3 * deltaT, Z_3x3;

  // Compute jacobian wrt acc noise
  Matrix dgpv_daccNoise = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, deltaVij_old,
          deltaRij_old, _1, measuredOmega, deltaT), measuredAcc);

  // Compute expected F wrt gyro noise
  Matrix dgpv_domegaNoise = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, deltaVij_old,
          deltaRij_old, measuredAcc, _1, deltaT), measuredOmega);

  // Compute expected f_rot wrt gyro noise
  Matrix dgr_dangle = numericalDerivative11<Rot3, Vector3>(
      boost::bind(&updatePreintegratedRot, deltaRij_old, _1, deltaT),
      measuredOmega);

  Matrix Gexpected(9, 9);
  Gexpected << //
      dgr_dangle, Z_3x3, Z_3x3, //
  dgpv_domegaNoise, dgpv_dintNoise, dgpv_daccNoise;

  EXPECT(assert_equal(Gexpected, Gactual));

  // Check covariance propagation
  Matrix9 measurementCovariance;
  measurementCovariance << //
      omegaNoiseVar * I_3x3, Z_3x3, Z_3x3, //
  Z_3x3, intNoiseVar * I_3x3, Z_3x3, //
  Z_3x3, Z_3x3, accNoiseVar * I_3x3;

  Matrix newPreintCovarianceExpected = Factual * oldPreintCovariance
      * Factual.transpose()
      + (1 / deltaT) * Gactual * measurementCovariance * Gactual.transpose();

  Matrix newPreintCovarianceActual = preintegrated.preintMeasCov();
  EXPECT(assert_equal(newPreintCovarianceExpected, newPreintCovarianceActual));
}

/* ************************************************************************* */
TEST(ImuFactor, JacobianPreintegratedCovariancePropagation_2ndOrderInt) {
  // Linearization point
  imuBias::ConstantBias bias; // Current estimate of acceleration and rotation rate biases

  // Measurements
  list<Vector3> measuredAccs, measuredOmegas;
  list<double> deltaTs;
  measuredAccs.push_back(Vector3(0.1, 0.0, 0.0));
  measuredOmegas.push_back(Vector3(M_PI / 100.0, 0.0, 0.0));
  deltaTs.push_back(0.01);
  measuredAccs.push_back(Vector3(0.1, 0.0, 0.0));
  measuredOmegas.push_back(Vector3(M_PI / 100.0, 0.0, 0.0));
  deltaTs.push_back(0.01);
  for (int i = 1; i < 100; i++) {
    measuredAccs.push_back(Vector3(0.05, 0.09, 0.01));
    measuredOmegas.push_back(
        Vector3(M_PI / 100.0, M_PI / 300.0, 2 * M_PI / 100.0));
    deltaTs.push_back(0.01);
  }
  // Actual preintegrated values
  PreintegratedImuMeasurements preintegrated =
      evaluatePreintegratedMeasurements(bias, measuredAccs, measuredOmegas,
          deltaTs);

  // so far we only created a nontrivial linearization point for the preintegrated measurements
  // Now we add a new measurement and ask for Jacobians
  const Vector3 newMeasuredAcc = Vector3(0.1, 0.0, 0.0);
  const Vector3 newMeasuredOmega = Vector3(M_PI / 100.0, 0.0, 0.0);
  const double newDeltaT = 0.01;
  const Rot3 deltaRij_old = preintegrated.deltaRij(); // before adding new measurement
  const Vector3 deltaVij_old = preintegrated.deltaVij(); // before adding new measurement
  const Vector3 deltaPij_old = preintegrated.deltaPij(); // before adding new measurement

  Matrix oldPreintCovariance = preintegrated.preintMeasCov();

  Matrix Factual, Gactual;
  preintegrated.integrateMeasurement(newMeasuredAcc, newMeasuredOmega,
      newDeltaT, Factual, Gactual);

  //////////////////////////////////////////////////////////////////////////////////////////////
  // COMPUTE NUMERICAL DERIVATIVES FOR F
  //////////////////////////////////////////////////////////////////////////////////////////////
  // Compute expected f_pos_vel wrt positions
  Matrix dfpv_dpos = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, _1, deltaVij_old, deltaRij_old,
          newMeasuredAcc, newMeasuredOmega, newDeltaT), deltaPij_old);

  // Compute expected f_pos_vel wrt velocities
  Matrix dfpv_dvel = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, _1, deltaRij_old,
          newMeasuredAcc, newMeasuredOmega, newDeltaT), deltaVij_old);

  // Compute expected f_pos_vel wrt angles
  Matrix dfpv_dangle = numericalDerivative11<Vector, Rot3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, deltaVij_old, _1,
          newMeasuredAcc, newMeasuredOmega, newDeltaT), deltaRij_old);

  // Compute expected f_rot wrt angles
  Matrix dfr_dangle = numericalDerivative11<Rot3, Rot3>(
      boost::bind(&updatePreintegratedRot, _1, newMeasuredOmega, newDeltaT),
      deltaRij_old);

  Matrix Fexpected(9, 9);
  Fexpected << //
      dfr_dangle, Z_3x3, Z_3x3, //
  dfpv_dangle, dfpv_dpos, dfpv_dvel;

  EXPECT(assert_equal(Fexpected, Factual));

  //////////////////////////////////////////////////////////////////////////////////////////////
  // COMPUTE NUMERICAL DERIVATIVES FOR G
  //////////////////////////////////////////////////////////////////////////////////////////////
  // Compute jacobian wrt integration noise
  Matrix dgpv_dintNoise(6, 3);
  dgpv_dintNoise << I_3x3 * newDeltaT, Z_3x3;

  // Compute jacobian wrt acc noise
  Matrix dgpv_daccNoise = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, deltaVij_old,
          deltaRij_old, _1, newMeasuredOmega, newDeltaT), newMeasuredAcc);

  // Compute expected F wrt gyro noise
  Matrix dgpv_domegaNoise = numericalDerivative11<Vector, Vector3>(
      boost::bind(&updatePreintegratedPosVel, deltaPij_old, deltaVij_old,
          deltaRij_old, newMeasuredAcc, _1, newDeltaT), newMeasuredOmega);

  // Compute expected f_rot wrt gyro noise
  Matrix dgr_dangle = numericalDerivative11<Rot3, Vector3>(
      boost::bind(&updatePreintegratedRot, deltaRij_old, _1, newDeltaT),
      newMeasuredOmega);

  Matrix Gexpected(9, 9);
  Gexpected << //
      dgr_dangle, Z_3x3, Z_3x3, //
  dgpv_domegaNoise, dgpv_dintNoise, dgpv_daccNoise;

  EXPECT(assert_equal(Gexpected, Gactual));

  // Check covariance propagation
  Matrix9 measurementCovariance;
  measurementCovariance << //
      omegaNoiseVar * I_3x3, Z_3x3, Z_3x3, //
      Z_3x3, intNoiseVar * I_3x3, Z_3x3,  //
      Z_3x3, Z_3x3, accNoiseVar * I_3x3;

  Matrix newPreintCovarianceExpected = Factual * oldPreintCovariance
      * Factual.transpose()
      + (1 / newDeltaT) * Gactual * measurementCovariance * Gactual.transpose();

  Matrix newPreintCovarianceActual = preintegrated.preintMeasCov();
  EXPECT(assert_equal(newPreintCovarianceExpected, newPreintCovarianceActual));
}

/* ************************************************************************* */
TEST(ImuFactor, ErrorWithBiasesAndSensorBodyDisplacement) {
  imuBias::ConstantBias bias(Vector3(0.2, 0, 0), Vector3(0, 0, 0.3)); // Biases (acc, rot)
  Pose3 x1(Rot3::Expmap(Vector3(0, 0, M_PI / 4.0)), Point3(5.0, 1.0, -50.0));
  Vector3 v1(Vector3(0.5, 0.0, 0.0));
  Pose3 x2(Rot3::Expmap(Vector3(0, 0, M_PI / 4.0 + M_PI / 10.0)),
      Point3(5.5, 1.0, -50.0));
  Vector3 v2(Vector3(0.5, 0.0, 0.0));

  // Measurements
  Vector3 measuredOmega;
  measuredOmega << 0, 0, M_PI / 10.0 + 0.3;
  Vector3 measuredAcc = x1.rotation().unrotate(-kGravityAlongNavZDown)
      + Vector3(0.2, 0.0, 0.0);
  double deltaT = 1.0;

  const Pose3 body_P_sensor(Rot3::Expmap(Vector3(0, 0.10, 0.10)),
      Point3(1, 0, 0));

  PreintegratedImuMeasurements pim(
      imuBias::ConstantBias(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.0)),
      kMeasuredAccCovariance, kMeasuredOmegaCovariance,
      kIntegrationErrorCovariance, true);

  pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT, body_P_sensor);

  Matrix expected(9,9);
  expected <<
      0.0290780477, 4.63277848e-07, 9.23468723e-05, 0.0, 0.0, 0.0,    0.0, 0.0, 0.0, //
      4.63277848e-07, 0.0290688208, 4.62505461e-06, 0.0, 0.0, 0.0,    0.0, 0.0, 0.0, //
      9.23468723e-05, 4.62505461e-06, 0.0299907267, 0.0, 0.0, 0.0,    0.0, 0.0, 0.0, //
      0.0, 0.0, 0.0,                                0.0026, 0.0, 0.0, 0.005, 0.0, 0.0, //
      0.0, 0.0, 0.0,                                0.0, 0.0026, 0.0, 0.0, 0.005, 0.0, //
      0.0, 0.0, 0.0,                                0.0, 0.0, 0.0026, 0.0, 0.0, 0.005, //
      0.0, 0.0, 0.0,                                0.005, 0.0, 0.0,  0.01, 0.0, 0.0, //
      0.0, 0.0, 0.0,                                0.0, 0.005, 0.0,  0.0, 0.01, 0.0, //
      0.0, 0.0, 0.0,                                0.0, 0.0, 0.005,  0.0, 0.0, 0.01;
  EXPECT(assert_equal(expected, pim.preintMeasCov(), 1e-6));

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kNonZeroOmegaCoriolis);

  // Predict
  Pose3 actual_x2;
  Vector3 actual_v2;
  ImuFactor::Predict(x1, v1, actual_x2, actual_v2, bias, pim,
      kGravityAlongNavZDown, kZeroOmegaCoriolis);

  // Regression test with
  Rot3 expectedR( //
      0.456795409,   -0.888128414,   0.0506544554,   //
      0.889548908,     0.45563417,   -0.0331699173,  //
     0.00637924528,  0.0602114814,    0.998165258);
  Point3 expectedT(5.30373101, 0.768972495, -49.9942188);
  Vector3 expected_v2(0.107462014, -0.46205501, 0.0115624037);
  Pose3 expected_x2(expectedR, expectedT);
  EXPECT(assert_equal(expected_x2, actual_x2, 1e-7));
  EXPECT(assert_equal(Vector(expected_v2), actual_v2, 1e-7));

  Values values;
  values.insert(X(1), x1);
  values.insert(V(1), v1);
  values.insert(X(2), x2);
  values.insert(V(2), v2);
  values.insert(B(1), bias);

  // Make sure linearization is correct
  double diffDelta = 1e-7;
  EXPECT_CORRECT_FACTOR_JACOBIANS(factor, values, diffDelta, 1e-3);
}

/* ************************************************************************* */
TEST(ImuFactor, PredictPositionAndVelocity) {
  imuBias::ConstantBias bias(Vector3(0, 0, 0), Vector3(0, 0, 0)); // Biases (acc, rot)

  // Measurements
  Vector3 measuredOmega;
  measuredOmega << 0, 0, 0; // M_PI/10.0+0.3;
  Vector3 measuredAcc;
  measuredAcc << 0, 1, -9.81;
  double deltaT = 0.001;

  PreintegratedImuMeasurements pim(
      imuBias::ConstantBias(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.0)),
      kMeasuredAccCovariance, kMeasuredOmegaCovariance,
      kIntegrationErrorCovariance, true);

  for (int i = 0; i < 1000; ++i)
    pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);

  // Predict
  Pose3 x1;
  Vector3 v1(0, 0.0, 0.0);
  PoseVelocityBias poseVelocity = pim.predict(x1, v1, bias,
      kGravityAlongNavZDown, kZeroOmegaCoriolis);
  Pose3 expectedPose(Rot3(), Point3(0, 0.5, 0));
  Vector3 expectedVelocity;
  expectedVelocity << 0, 1, 0;
  EXPECT(assert_equal(expectedPose, poseVelocity.pose));
  EXPECT(assert_equal(Vector(expectedVelocity), Vector(poseVelocity.velocity)));
}

/* ************************************************************************* */
TEST(ImuFactor, PredictRotation) {
  imuBias::ConstantBias bias(Vector3(0, 0, 0), Vector3(0, 0, 0)); // Biases (acc, rot)

  // Measurements
  Vector3 measuredOmega;
  measuredOmega << 0, 0, M_PI / 10; // M_PI/10.0+0.3;
  Vector3 measuredAcc;
  measuredAcc << 0, 0, -9.81;
  double deltaT = 0.001;

  PreintegratedImuMeasurements pim(
      imuBias::ConstantBias(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.0)),
      kMeasuredAccCovariance, kMeasuredOmegaCovariance,
      kIntegrationErrorCovariance, true);

  for (int i = 0; i < 1000; ++i)
    pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);

  // Predict
  Pose3 x1, x2;
  Vector3 v1 = Vector3(0, 0.0, 0.0);
  Vector3 v2;
  ImuFactor::Predict(x1, v1, x2, v2, bias, pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);
  Pose3 expectedPose(Rot3().ypr(M_PI / 10, 0, 0), Point3(0, 0, 0));
  Vector3 expectedVelocity;
  expectedVelocity << 0, 0, 0;
  EXPECT(assert_equal(expectedPose, x2));
  EXPECT(assert_equal(Vector(expectedVelocity), Vector(v2)));
}

/* ************************************************************************* */
TEST(ImuFactor, PredictArbitrary) {
  imuBias::ConstantBias bias(Vector3(0, 0, 0), Vector3(0, 0, 0)); // Biases (acc, rot)

  // Measurements
  Vector3 measuredOmega(M_PI / 10, M_PI / 10, M_PI / 10);
  Vector3 measuredAcc(0.1, 0.2, -9.81);
  double deltaT = 0.001;

  ImuFactor::PreintegratedMeasurements pim(
      imuBias::ConstantBias(Vector3(0.2, 0.0, 0.0), Vector3(0.0, 0.0, 0.0)),
      kMeasuredAccCovariance, kMeasuredOmegaCovariance,
      kIntegrationErrorCovariance, true);

  for (int i = 0; i < 1000; ++i)
    pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT);

  Matrix expected(9,9);
  expected << //
      0.0299999995, 2.46739898e-10, 2.46739896e-10, -0.0144839494, 0.044978128, 0.0100471195, -0.0409843415, 0.134423822, 0.0383280513, //
      2.46739898e-10, 0.0299999995, 2.46739902e-10, -0.0454268484, -0.0149428917, 0.00609093775, -0.13554868, -0.0471183681, 0.0247643646, //
      2.46739896e-10, 2.46739902e-10, 0.0299999995, 0.00489577218, 0.00839301168, 0.000448720395, 0.00879031682, 0.0162199769, 0.00112485862, //
     -0.0144839494, -0.0454268484, 0.00489577218, 0.142448905, 0.00345595825, -0.0225794125, 0.34774305, 0.0119449979, -0.075667905, //
      0.044978128, -0.0149428917, 0.00839301168, 0.00345595825, 0.143318431, 0.0200549262, 0.0112877167, 0.351503176, 0.0629164907, //
      0.0100471195, 0.00609093775, 0.000448720395, -0.0225794125, 0.0200549262, 0.0104041905, -0.0580647212, 0.051116506, 0.0285371399, //
     -0.0409843415, -0.13554868, 0.00879031682, 0.34774305, 0.0112877167, -0.0580647212, 0.911721561, 0.0412249672, -0.205920425, //
      0.134423822, -0.0471183681, 0.0162199769, 0.0119449979, 0.351503176, 0.051116506, 0.0412249672, 0.928013807, 0.169935105, //
      0.0383280513, 0.0247643646, 0.00112485862, -0.075667905, 0.0629164907, 0.0285371399, -0.205920425, 0.169935105, 0.09407764;
  EXPECT(assert_equal(expected, pim.preintMeasCov(), 1e-7));

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);

  // Predict
  Pose3 x1, x2;
  Vector3 v1 = Vector3(0, 0.0, 0.0);
  Vector3 v2;
  ImuFactor::Predict(x1, v1, x2, v2, bias, pim, kGravityAlongNavZDown,
      kZeroOmegaCoriolis);

  // Regression test for Imu Refactor
  Rot3 expectedR( //
      +0.903715275, -0.250741668, 0.347026393, //
      +0.347026393, 0.903715275, -0.250741668, //
      -0.250741668, 0.347026393, 0.903715275);
  Point3 expectedT(-0.505517319, 0.569413747, 0.0861035711);
  Vector3 expectedV(-1.59121524, 1.55353139, 0.3376838540);
  Pose3 expectedPose(expectedR, expectedT);
  EXPECT(assert_equal(expectedPose, x2, 1e-7));
  EXPECT(assert_equal(Vector(expectedV), v2, 1e-7));
}

/* ************************************************************************* */
TEST(ImuFactor, bodyPSensorNoBias) {
  imuBias::ConstantBias bias(Vector3(0, 0, 0), Vector3(0, 0.1, 0)); // Biases (acc, rot)

  // Measurements
  Vector3 n_gravity(0, 0, -9.81); // z-up nav frame
  Vector3 omegaCoriolis(0, 0, 0);
  // Sensor frame is z-down
  // Gyroscope measurement is the angular velocity of sensor w.r.t nav frame in sensor frame
  Vector3 s_omegaMeas_ns(0, 0.1, M_PI / 10);
  // Acc measurement is acceleration of sensor in the sensor frame, when stationary,
  // table exerts an equal and opposite force w.r.t gravity
  Vector3 s_accMeas(0, 0, -9.81);
  double dt = 0.001;

  // Rotate sensor (z-down) to body (same as navigation) i.e. z-up
  Pose3 body_P_sensor(Rot3::ypr(0, 0, M_PI), Point3(0, 0, 0));

  ImuFactor::PreintegratedMeasurements pim(bias, Z_3x3, Z_3x3, Z_3x3, true);

  for (int i = 0; i < 1000; ++i)
    pim.integrateMeasurement(s_accMeas, s_omegaMeas_ns, dt, body_P_sensor);

  // Create factor
  ImuFactor factor(X(1), V(1), X(2), V(2), B(1), pim, n_gravity, omegaCoriolis);

  // Predict
  Pose3 x1;
  Vector3 v1(0, 0, 0);
  PoseVelocityBias poseVelocity = pim.predict(x1, v1, bias, n_gravity,
      omegaCoriolis);

  Pose3 expectedPose(Rot3().ypr(-M_PI / 10, 0, 0), Point3(0, 0, 0));
  EXPECT(assert_equal(expectedPose, poseVelocity.pose));

  Vector3 expectedVelocity(0, 0, 0);
  EXPECT(assert_equal(Vector(expectedVelocity), Vector(poseVelocity.velocity)));
}

/* ************************************************************************* */
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>

TEST(ImuFactor, bodyPSensorWithBias) {
  using noiseModel::Diagonal;
  typedef imuBias::ConstantBias Bias;

  int numFactors = 80;
  Vector6 noiseBetweenBiasSigma;
  noiseBetweenBiasSigma << Vector3(2.0e-5, 2.0e-5, 2.0e-5), Vector3(3.0e-6,
      3.0e-6, 3.0e-6);
  SharedDiagonal biasNoiseModel = Diagonal::Sigmas(noiseBetweenBiasSigma);

  // Measurements
  Vector3 n_gravity(0, 0, -9.81);
  Vector3 omegaCoriolis(0, 0, 0);

  // Sensor frame is z-down
  // Gyroscope measurement is the angular velocity of sensor w.r.t nav frame in sensor frame
  Vector3 measuredOmega(0, 0.01, 0);
  // Acc measurement is acceleration of sensor in the sensor frame, when stationary,
  // table exerts an equal and opposite force w.r.t gravity
  Vector3 measuredAcc(0, 0, -9.81);

  Pose3 body_P_sensor(Rot3::ypr(0, 0, M_PI), Point3());

  Matrix3 accCov = 1e-7 * I_3x3;
  Matrix3 gyroCov = 1e-8 * I_3x3;
  Matrix3 integrationCov = 1e-9 * I_3x3;
  double deltaT = 0.005;

  //   Specify noise values on priors
  Vector6 priorNoisePoseSigmas(
      (Vector(6) << 0.001, 0.001, 0.001, 0.01, 0.01, 0.01).finished());
  Vector3 priorNoiseVelSigmas((Vector(3) << 0.1, 0.1, 0.1).finished());
  Vector6 priorNoiseBiasSigmas(
      (Vector(6) << 0.1, 0.1, 0.1, 0.5e-1, 0.5e-1, 0.5e-1).finished());
  SharedDiagonal priorNoisePose = Diagonal::Sigmas(priorNoisePoseSigmas);
  SharedDiagonal priorNoiseVel = Diagonal::Sigmas(priorNoiseVelSigmas);
  SharedDiagonal priorNoiseBias = Diagonal::Sigmas(priorNoiseBiasSigmas);
  Vector3 zeroVel(0, 0, 0);

  // Create a factor graph with priors on initial pose, vlocity and bias
  NonlinearFactorGraph graph;
  Values values;

  PriorFactor<Pose3> priorPose(X(0), Pose3(), priorNoisePose);
  graph.add(priorPose);
  values.insert(X(0), Pose3());

  PriorFactor<Vector3> priorVel(V(0), zeroVel, priorNoiseVel);
  graph.add(priorVel);
  values.insert(V(0), zeroVel);

  // The key to this test is that we specify the bias, in the sensor frame, as known a priori
  // We also create factors below that encode our assumption that this bias is constant over time
  // In theory, after optimization, we should recover that same bias estimate
  Bias priorBias(Vector3(0, 0, 0), Vector3(0, 0.01, 0)); // Biases (acc, rot)
  PriorFactor<Bias> priorBiasFactor(B(0), priorBias, priorNoiseBias);
  graph.add(priorBiasFactor);
  values.insert(B(0), priorBias);

  // Now add IMU factors and bias noise models
  Bias zeroBias(Vector3(0, 0, 0), Vector3(0, 0, 0));
  for (int i = 1; i < numFactors; i++) {
    ImuFactor::PreintegratedMeasurements pim =
        ImuFactor::PreintegratedMeasurements(zeroBias, accCov, gyroCov,
            integrationCov, true);
    for (int j = 0; j < 200; ++j)
      pim.integrateMeasurement(measuredAcc, measuredOmega, deltaT,
          body_P_sensor);

    // Create factors
    graph.add(
        ImuFactor(X(i - 1), V(i - 1), X(i), V(i), B(i - 1), pim, n_gravity,
            omegaCoriolis));
    graph.add(BetweenFactor<Bias>(B(i - 1), B(i), zeroBias, biasNoiseModel));

    values.insert(X(i), Pose3());
    values.insert(V(i), zeroVel);
    values.insert(B(i), priorBias);
  }

  // Finally, optimize, and get bias at last time step
  Values results = LevenbergMarquardtOptimizer(graph, values).optimize();
  Bias biasActual = results.at<Bias>(B(numFactors - 1));

  // And compare it with expected value (our prior)
  Bias biasExpected(Vector3(0, 0, 0), Vector3(0, 0.01, 0));
  EXPECT(assert_equal(biasExpected, biasActual, 1e-3));
}

/* ************************************************************************* */
int main() {
  TestResult tr;
  return TestRegistry::runAllTests(tr);
}
/* ************************************************************************* */
