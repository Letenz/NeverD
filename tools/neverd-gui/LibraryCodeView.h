#pragma once

#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVector>

// A reversible projection of one loaded source page. UTF-8 byte offsets are
// validated before conversion to Qt's UTF-16 coordinates. The worker's original
// text remains the only source for copying/exporting or instruction navigation.
class LibraryCodeView final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString text READ text NOTIFY changed)
  Q_PROPERTY(QVariantList mappings READ mappings NOTIFY changed)
  Q_PROPERTY(QVariantList regions READ regions NOTIFY changed)
  Q_PROPERTY(int foldableCount READ foldableCount NOTIFY changed)
  Q_PROPERTY(bool anyFolded READ anyFolded NOTIFY changed)
public:
  explicit LibraryCodeView(QObject *parent = nullptr) : QObject(parent) {}
  void reset(const QString &original, const QVariantList &mappings,
             const QJsonArray &regions, qint64 byteOffset = 0,
             bool keepFolded = false);
  QString text() const { return text_; }
  QVariantList mappings() const { return mappings_; }
  QVariantList regions() const;
  int foldableCount() const;
  bool anyFolded() const { return !folded_.isEmpty(); }
  Q_INVOKABLE void toggleRegion(const QString &id);
  Q_INVOKABLE void setFolded(bool folded);
  Q_INVOKABLE QString regionAt(int position) const;
  Q_INVOKABLE int sourceLineAt(int position) const;
  Q_INVOKABLE QString originalSelection(int begin, int end) const;
  Q_INVOKABLE void copySelection(int begin, int end) const;
signals:
  void changed();

private:
  struct Span {
    int begin = 0, end = 0;
  };
  struct Region {
    QVariantMap details;
    QString id, summary;
    QVector<Span> spans;
    bool available = false;
  };
  struct Segment {
    int begin, end, sourceBegin, sourceEnd;
    QString region;
  };
  int sourcePosition(int position, bool end) const;
  int displayPosition(int position) const;
  void rebuild();
  QString original_, text_;
  QVariantList originalMappings_, mappings_;
  QVector<Region> regions_;
  QVector<Segment> segments_;
  QSet<QString> folded_;
};
