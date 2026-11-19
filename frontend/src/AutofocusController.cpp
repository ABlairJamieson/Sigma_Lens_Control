#include "AutofocusController.h"
#include <algorithm>
#include <cmath>

AutofocusController::AutofocusController(QObject* parent) : QObject(parent) {
  mDeadline.setSingleShot(true); mSettle.setSingleShot(true); mTotal.setSingleShot(true);
  connect(&mDeadline,&QTimer::timeout,this,[this] {
    if (mState == Returning || mState == ReturningBackoff) {
      mReturnSuccess=false; mReturnMessage += "; lens position could not be verified"; complete();
    } else cancel("Autofocus timed out waiting for lens/frame");
  });
  connect(&mTotal,&QTimer::timeout,this,[this] { cancel("Autofocus exceeded three minutes"); });
  connect(&mSettle,&QTimer::timeout,this,[this] {
    if (mState != Settling) return;
    mState=Sampling; mSamples.clear();
    mToken=QString::number(++mGeneration);
    mDeadline.start(20000);
    emit freshFrameRequested(mToken);
  });
}
void AutofocusController::grid(int low,int high) {
  mPositions.clear();
  for (int i=0;i<9;++i) {
    int p=low+(high-low)*i/8;
    if (mPositions.isEmpty() || mPositions.last()!=p) mPositions.append(p);
  }
  mIndex=0;
}
void AutofocusController::start(int current,int radius) {
  if (active()) return;
  if (current<8232 || current>12777) {
    emit finished(false,current,"Lens position outside supported range; no movement"); return;
  }
  mOriginal=std::clamp(current,8232,12777); mLastPosition=mOriginal;
  mLow=std::max(8232,mOriginal-std::clamp(radius,16,4545));
  mHigh=std::min(12777,mOriginal+std::clamp(radius,16,4545));
  mStage=0; mBest=mOriginal; mBestScore=-1; mWorstScore=1e300;
  grid(mLow,mHigh); mTotal.start(180000); nextPosition();
}
void AutofocusController::nextPosition() {
  if (mIndex >= mPositions.size()) {
    if (mStage == 0) {
      const int step=std::max(1,(mHigh-mLow)/8);
      grid(std::max(mLow,mBest-step),std::min(mHigh,mBest+step));
      mStage=1; nextPosition(); return;
    }
    if (mBestScore <= 1 || mBestScore < mWorstScore*1.05) {
      cancel("Insufficient focus contrast"); return;
    }
    returnTo(mBest,true,QString("Autofocus complete: raw %1, sharpness %2%3")
      .arg(mBest).arg(mBestScore,0,'f',1)
      .arg(mBest==mLow || mBest==mHigh ? " (range edge — widen search or check manually)" : ""));
    return;
  }
  mTarget=mPositions[mIndex];
  // Approach every measured position from the same direction to reduce backlash.
  mAckTarget=std::max(8232,mTarget-32); mState=Backoff; mDeadline.start(10000);
  emit progress(QString("Autofocus %1: %2/%3, raw %4").arg(mStage ? "fine" : "coarse")
                .arg(mIndex+1).arg(mPositions.size()).arg(mTarget));
  emit moveRequested(mAckTarget);
}
void AutofocusController::positionReceived(int position) {
  if (!active()) return;
  mLastPosition=position;
  if (position!=mAckTarget) return;
  if (mState==Returning) { mPositionConfirmed=true; complete(); return; }
  if (mState==ReturningBackoff) {
    mState=Returning; mAckTarget=mTarget; mDeadline.start(10000); emit moveRequested(mTarget); return;
  }
  if (mState==Backoff) {
    mState=Moving; mAckTarget=mTarget; mDeadline.start(10000); emit moveRequested(mTarget);
  } else if (mState==Moving) {
    mState=Settling; mDeadline.start(20000); mSettle.start(350);
  }
}
void AutofocusController::frameReceived(const QJsonObject& frame) {
  if (mState!=Sampling || frame.value("focus_token").toString()!=mToken) return;
  const double score=frame.value("focus_score").toDouble(-1);
  const double level=frame.value("mean_level").toDouble(-1);
  if (frame.value("view_mode").toString()!="focus" || !std::isfinite(score) || score<0 ||
      level<5 || level>245 || frame.value("saturated_fraction").toDouble(1)>.05) {
    cancel("Focus image is dark, saturated or unsupported; adjust exposure and retry"); return;
  }
  mSamples.append(score);
  if (mSamples.size()<3) return;
  std::sort(mSamples.begin(),mSamples.end());
  const double median=mSamples[1];
  if (median>mBestScore) { mBestScore=median; mBest=mTarget; }
  mWorstScore=std::min(mWorstScore,median);
  emit progress(QString("Autofocus %1 sample: raw %2, sharpness %3; best raw %4, sharpness %5")
    .arg(mStage ? "fine" : "coarse").arg(mTarget).arg(median,0,'f',1).arg(mBest).arg(mBestScore,0,'f',1));
  ++mIndex; nextPosition();
}
void AutofocusController::returnTo(int position,bool success,const QString& message) {
  mSettle.stop(); mTotal.stop(); mReturnMessage=message; mReturnSuccess=success;
  mPositionConfirmed=false;
  emit progress(QString("%1: returning to %2 raw %3; waiting for lens confirmation")
    .arg(success ? "Autofocus search finished" : message).arg(success ? "best focus" : "original focus").arg(position));
  mTarget=position; mAckTarget=std::max(8232,position-32);
  mState=ReturningBackoff; mDeadline.start(10000);
  emit moveRequested(mAckTarget);
}
void AutofocusController::cancel(const QString& reason) {
  if (!active()) return;
  if ((mState==Returning || mState==ReturningBackoff) && !mReturnSuccess) return;
  returnTo(mOriginal,false,reason);
}
void AutofocusController::complete() {
  mDeadline.stop(); mSettle.stop(); mTotal.stop(); mState=Idle;
  if (mPositionConfirmed) {
    mReturnMessage += mReturnSuccess ? QString("; lens confirmed raw %1").arg(mTarget)
      : QString("; original focus restored at raw %1 (lens confirmed)").arg(mTarget);
  }
  emit finished(mReturnSuccess,mPositionConfirmed ? mTarget : mLastPosition,mReturnMessage);
}
