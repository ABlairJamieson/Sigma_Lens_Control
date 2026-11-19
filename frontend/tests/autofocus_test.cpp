#include <QtTest>
#include <QSignalSpy>
#include "AutofocusController.h"

class AutofocusTest : public QObject {
  Q_OBJECT
private slots:
  void findsPeakWithFreshFramesAndAcknowledgedMoves() {
    AutofocusController af;
    int position=0; QVector<int> moves;
    connect(&af,&AutofocusController::moveRequested,&af,[&](int p) {
      position=p; moves.append(p);
      QTimer::singleShot(0,&af,[&,p] { af.positionReceived(p); });
    });
    connect(&af,&AutofocusController::freshFrameRequested,&af,[&](const QString& token) {
      const double score=1000.0-(position-9020)*(position-9020)*.1;
      QJsonObject sample{{"focus_token",token},{"view_mode","focus"},{"focus_score",std::max(10.0,score)},
                         {"mean_level",100},{"saturated_fraction",0.0}};
      auto stale=sample; stale["focus_token"]="stale";
      af.frameReceived(stale); // must not influence the search
      for(int i=0;i<3;++i) af.frameReceived(sample);
    });
    QSignalSpy done(&af,&AutofocusController::finished);
    af.start(9000,64);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,15000);
    QVERIFY(done[0][0].toBool()); QCOMPARE(done[0][1].toInt(),9020);
    QVERIFY(!af.active());
    QVERIFY(done[0][2].toString().contains("lens confirmed raw 9020"));
    QCOMPARE(moves[moves.size()-2],8988); QCOMPARE(moves.last(),9020);
    for(int p:moves) QVERIFY(p>=8904 && p<=9064);
  }
  void cancelRestoresOriginalOnlyAfterAcknowledgement() {
    AutofocusController af; QSignalSpy moves(&af,&AutofocusController::moveRequested);
    QSignalSpy done(&af,&AutofocusController::finished);
    af.start(9000,64); QVERIFY(af.active());
    af.cancel(); QCOMPARE(moves.last()[0].toInt(),8968);
    af.positionReceived(9000); QCOMPARE(done.count(),0); // wrong phase/position ignored
    af.positionReceived(8968); QCOMPARE(moves.last()[0].toInt(),9000);
    QCOMPARE(done.count(),0); af.positionReceived(9000);
    QCOMPARE(done.count(),1); QVERIFY(!done[0][0].toBool());
    QVERIFY(done[0][2].toString().contains("original focus restored at raw 9000 (lens confirmed)"));
  }
  void failedReturnDoesNotClaimOriginalWasRestored() {
    AutofocusController af;
    QSignalSpy done(&af,&AutofocusController::finished);
    af.start(9000,64); af.cancel();
    af.positionReceived(8968); // backoff reached, final 9000 requested
    af.positionReceived(8970); // never reached requested position
    QCOMPARE(done.count(),0);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,12000);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(done[0][1].toInt(),8970);
    QVERIFY(done[0][2].toString().contains("could not be verified"));
    QVERIFY(!done[0][2].toString().contains("original focus restored"));
  }
  void invalidImageRestoresOriginal() {
    AutofocusController af;
    connect(&af,&AutofocusController::moveRequested,&af,[&](int p) {
      QTimer::singleShot(0,&af,[&,p] { af.positionReceived(p); });
    });
    connect(&af,&AutofocusController::freshFrameRequested,&af,[&](const QString& token) {
      af.frameReceived(QJsonObject{{"focus_token",token},{"view_mode","focus"},
        {"focus_score",500},{"mean_level",254},{"saturated_fraction",0.9}});
    });
    QSignalSpy done(&af,&AutofocusController::finished); af.start(9000,64);
    QTRY_COMPARE(done.count(),1); QVERIFY(!done[0][0].toBool()); QCOMPARE(done[0][1].toInt(),9000);
  }
  void flatSceneDoesNotClaimSuccess() {
    AutofocusController af;
    connect(&af,&AutofocusController::moveRequested,&af,[&](int p) {
      QTimer::singleShot(0,&af,[&,p] { af.positionReceived(p); });
    });
    connect(&af,&AutofocusController::freshFrameRequested,&af,[&](const QString& token) {
      const QJsonObject sample{{"focus_token",token},{"view_mode","focus"},
        {"focus_score",100},{"mean_level",100},{"saturated_fraction",0.0}};
      for(int i=0;i<3;++i) af.frameReceived(sample);
    });
    QSignalSpy done(&af,&AutofocusController::finished); af.start(9000,64);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,15000);
    QVERIFY(!done[0][0].toBool()); QCOMPARE(done[0][1].toInt(),9000);
    QVERIFY(done[0][2].toString().contains("Insufficient focus contrast"));
  }
  void invalidLensPositionCannotMove() {
    AutofocusController af; QSignalSpy moves(&af,&AutofocusController::moveRequested);
    QSignalSpy done(&af,&AutofocusController::finished); af.start(0,64);
    QCOMPARE(moves.count(),0); QCOMPARE(done.count(),1); QVERIFY(!done[0][0].toBool());
  }
  void missingPositionFeedbackCannotScoreFrames() {
    AutofocusController af; QSignalSpy requests(&af,&AutofocusController::freshFrameRequested);
    af.start(9000,64); af.positionReceived(10000); QTest::qWait(400);
    QCOMPARE(requests.count(),0); QVERIFY(af.active()); af.cancel();
  }
};
QTEST_GUILESS_MAIN(AutofocusTest)
#include "autofocus_test.moc"
