#include "LibraryCodeView.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QTest>

namespace {
QJsonObject region(QString id, qint64 begin, qint64 end) {
  return {{"id", id},
          {"display_name", "std::vector<unsigned int>::size"},
          {"foldable", true},
          {"mapping_status", "mapped"},
          {"spans",
           QJsonArray{QJsonObject{{"begin_byte", begin}, {"end_byte", end}}}}};
}
} // namespace

class LibraryCodeViewTests final : public QObject {
  Q_OBJECT
private slots:
  void foldUnfoldAndCopyUseOriginalUtf8() {
    LibraryCodeView view;
    const QString source =
        QString::fromUtf8("/* 中文 😀 */\nreturn end - begin;\nnext();\n");
    const int begin = source.indexOf("end");
    const int end = source.indexOf(';');
    const auto item = region("one", source.left(begin).toUtf8().size(),
                             source.left(end).toUtf8().size());
    view.reset(source, {}, {item});
    QCOMPARE(view.text(), source);
    QCOMPARE(view.foldableCount(), 1);
    view.setFolded(true);
    QVERIFY(view.anyFolded());
    QVERIFY(!view.text().contains("end - begin"));
    QCOMPARE(view.regionAt(begin), QString("one"));
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    const int summaryEnd = view.text().indexOf(';');
    QCOMPARE(view.originalSelection(begin + 3, summaryEnd - 2),
             QString("end - begin"));
    QCOMPARE(view.sourceLineAt(summaryEnd + 2), 2);
    view.copySelection(0, view.text().size());
    QCOMPARE(QGuiApplication::clipboard()->text(), source);
    view.toggleRegion("one");
    QCOMPARE(view.text(), source);
    QVERIFY(!view.anyFolded());
  }

  void disjointSpansPreserveInterleavedCodeAndNavigation() {
    LibraryCodeView view;
    const QString source = "first\nkeep\nlast\ntail\n";
    auto item = region("one", 0, 6);
    item["spans"] =
        QJsonArray{QJsonObject{{"begin_byte", 0}, {"end_byte", 6}},
                   QJsonObject{{"begin_byte", 11}, {"end_byte", 16}}};
    const QVariantList rows{
        QVariantMap{{"line", 3}, {"addresses", QVariantList{"0x1234"}}}};
    view.reset(source, rows, {item});
    view.setFolded(true);
    QVERIFY(view.text().contains("keep\n"));
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    const int tail = view.text().indexOf("tail");
    QCOMPARE(view.sourceLineAt(tail), 3);
    QCOMPARE(view.mappings().first().toMap()["line"].toInt(),
             view.text().left(tail).count('\n'));
    view.setFolded(false);
    QCOMPARE(view.mappings(), rows);
    QCOMPARE(view.text(), source);
  }

  void incompleteAndAmbiguousSpansStayExpanded() {
    LibraryCodeView view;
    const QString source = "0123456789";
    for (const QJsonArray &items :
         {QJsonArray{region("outside", 2, 11)},
          QJsonArray{region("one", 1, 6), region("two", 4, 8)},
          QJsonArray{region("same", 1, 3), region("same", 6, 8)}}) {
      view.reset(source, {}, items);
      QCOMPARE(view.foldableCount(), 0);
      view.setFolded(true);
      QCOMPARE(view.text(), source);
    }
    auto unknown = region("unknown", 0, 6);
    unknown["mapping_status"] = "unknown";
    view.reset(source, {}, {unknown});
    QCOMPARE(view.foldableCount(), 0);
  }

  void byteBoundariesDoNotSplitCharacters() {
    LibraryCodeView view;
    view.reset(QString::fromUtf8("甲乙"), {}, {region("one", 1, 4)});
    QCOMPARE(view.foldableCount(), 0);
    auto fractional = region("one", 0, 3);
    fractional["spans"] =
        QJsonArray{QJsonObject{{"begin_byte", 0.5}, {"end_byte", 3}}};
    view.reset(QString::fromUtf8("甲乙"), {}, {fractional});
    QCOMPARE(view.foldableCount(), 0);
    view.reset(QString::fromUtf8("甲乙"), {}, {region("one", 100, 103)}, 100);
    QCOMPARE(view.foldableCount(), 1);
    view.setFolded(true);
    QVERIFY(view.text().endsWith(QString::fromUtf8("乙")));
  }

  void appendAndReplacementInvalidateUnavailableRegions() {
    LibraryCodeView view;
    view.reset("01234", {}, {region("one", 0, 3)});
    view.setFolded(true);
    view.reset("0123456789", {}, {region("one", 0, 3)}, 0, true);
    QVERIFY(view.anyFolded());
    view.reset("0123456789", {}, {});
    QVERIFY(!view.anyFolded());
    QCOMPARE(view.text(), QString("0123456789"));
    view.reset("replacement", {}, {region("one", 0, 3)});
    QVERIFY(!view.anyFolded());
  }
};

QTEST_MAIN(LibraryCodeViewTests)
#include "LibraryCodeViewTests.moc"
