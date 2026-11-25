#pragma once
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QVector>
#include <QJsonObject>

// Bounded contrast search. Motion is acknowledged by fresh lens telemetry;
// only token-matched, uncompressed focus samples are scored.
class AutofocusController : public QObject {
  Q_OBJECT
public:
  explicit AutofocusController(QObject* parent = nullptr);
  bool active() const { return mState != Idle; }
  void start(int currentPosition, int radius);
  void cancel(const QString& reason = "Autofocus cancelled");
  void positionReceived(int position);
  void frameReceived(const QJsonObject& frame);
signals:
  void moveRequested(int position);
  void freshFrameRequested(const QString& token);
  void progress(const QString& message);
  void finished(bool success, int position, const QString& message);
private:
  enum State { Idle, Backoff, Moving, Settling, Sampling, ReturningBackoff, Returning } mState = Idle;
  QTimer mDeadline, mSettle, mTotal;
  QElapsedTimer mPhaseTime;
  qint64 mMoveMs=0, mSettleMs=0, mFirstFrameMs=0;
  double mAcquisitionMs=0;
  bool mVerifying=false;
  QVector<int> mPositions;
  QVector<double> mSamples;
  int mOriginal=0, mLow=0, mHigh=0, mIndex=0, mTarget=0, mAckTarget=0, mStage=0, mBest=0;
  double mBestScore=-1, mWorstScore=1e300;
  QString mToken, mReturnMessage;
  bool mReturnSuccess=false, mPositionConfirmed=false;
  int mLastPosition=0;
  quint64 mGeneration=0;
  void grid(int low, int high);
  void nextPosition();
  void returnTo(int position, bool success, const QString& message);
  void complete();
};
