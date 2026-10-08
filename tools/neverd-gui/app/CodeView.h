#pragma once

#include "Address.h"
#include "LibraryCodeView.h"
#include "StyledText.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <optional>

class QComboBox;
class QLabel;
class QToolButton;

namespace neverd::gui {

class Session;

/// Text of one function in a code representation: C pseudocode (native or
/// through LLVM) or a textual IR.  Lines are fetched in pages and colored
/// locally; rows the worker maps to instructions synchronize with the
/// disassembly cursor.
class CodeText final : public QAbstractScrollArea {
  Q_OBJECT
public:
  struct Line {
    StyledLine styled;
    QVector<Address> addresses;
    /// The line shows a folded library operation.
    bool folded = false;
  };

  CodeText(Session &session, QWidget *parent = nullptr);

  void load(Address function, const QString &representation);
  void clear();
  std::optional<Address> function() const { return function_; }
  QString representation() const { return representation_; }
  /// First instruction address mapped to the cursor line.
  std::optional<Address> currentAddress() const;
  /// Highlight rows mapped to \p address and reveal the first.
  void revealAddress(Address address);
  QString currentToken() const;
  QString selectedText() const;
  QString allText() const;
  bool findText(const QString &text, bool forward);
  /// Move the cursor to \p line, revealing it and following its mapping.
  void setCursorLine(int line) { moveCursor(line, 0, false); }
  int lineCount() const { return int(lines_.size()); }
  /// Recognized library operations that can fold into one-line summaries.
  int foldableCount() const { return library_.foldableCount(); }
  bool anyFolded() const { return library_.anyFolded(); }
  /// Some recognized library operation shows as its summary.
  bool libraryFolded() const { return library_.libraryFolded(); }
  /// C code with the includes and declarations before its definition.
  bool hasPrelude() const;
  bool preludeFolded() const;
  void setPreludeFolded(bool folded);
  /// The source line that declares type or macro \p name in this code: a
  /// typedef, struct, union, enum or #define.  Functions are named in the
  /// binary and navigate there instead.
  std::optional<int> declarationLine(const QString &name) const;
  /// Show the declaration of \p name, expanding the prelude that holds it.
  bool goToDeclaration(const QString &name);
  /// The symbol a function declaration links \p name to with an assembler
  /// label: a C++ function reads by its stem and links by its mangled name.
  std::optional<QString> linkedSymbol(const QString &name) const;
  /// The image address of a global the code declares, as the comment its
  /// writer puts beside the declaration names it.
  std::optional<Address> objectAddress(const QString &name) const;
  void setFolded(bool folded);
  const QString &status() const { return status_; }
  /// Pages of the current function are still arriving.
  bool loading() const { return loading_; }

signals:
  void locationChanged(neverd::gui::Address address);
  /// A name was double-clicked; the owner resolves and navigates.
  void nameActivated(const QString &name);
  /// A global the code declares at an image address was double-clicked.
  void objectActivated(neverd::gui::Address address);
  void statusChanged();
  void foldingChanged();
  void contextMenuRequested(const QPoint &globalPosition);

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;
  bool event(QEvent *event) override;

private:
  void request(int offset, quint64 serial);
  void appendPage(const QJsonObject &payload, int offset);
  /// The library regions of the loaded function and its prelude.
  QJsonArray foldRegions() const;
  /// Display lines from the source, folded where the user asked.
  void rebuildLines();
  /// Position in the displayed text of a line and column.
  int displayPosition(int line, int column) const;
  QString regionAt(const QPoint &position) const;
  void highlightLine(Line &line, bool &inComment) const;
  void updateMetrics();
  void updateRange();
  int visibleLines() const;
  int lineAt(int y) const;
  int columnAt(int line, int x) const;
  int textLeft() const;
  void moveCursor(int line, int column, bool extend);

  Session &session_;
  QVector<Line> lines_;
  QVector<int> lineStarts_;
  // The function's complete source text and its row mappings; library
  // folding projects them, copy and export use them unchanged.
  QString source_;
  QVariantList sourceRows_;
  QJsonArray regions_;
  qint64 byteOffset_ = 0;
  bool regionsValid_ = false;
  LibraryCodeView library_;
  std::optional<Address> function_;
  QString representation_, status_, highlight_;
  quint64 serial_ = 0;
  bool inComment_ = false, loading_ = false, foldAfterLoad_ = false;
  bool foldPreludeAfterLoad_ = true;
  /// The first page's prelude: lines and end_byte.
  QJsonObject prelude_;
  /// What the code declares, indexed on first use.
  struct Declarations {
    /// Types and macros, by their source lines.
    QHash<QString, int> types;
    /// Functions declared with an assembler label: source line and symbol.
    QHash<QString, std::pair<int, QString>> linked;
    /// Globals declared at image addresses.
    QHash<QString, Address> objects;
  };
  const Declarations &declarations() const;
  mutable std::optional<Declarations> declarations_;
  int cursorLine_ = 0, cursorColumn_ = 0;
  std::optional<std::pair<int, int>> anchor_;
  QVector<int> marked_;
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12, gutterChars_ = 4;
  quint64 styleStamp_ = 1;
};

/// A pseudocode or IR window: the representation selector over a CodeText.
class CodeView final : public QWidget {
  Q_OBJECT
public:
  CodeView(Session &session, const QString &representation,
           QWidget *parent = nullptr);
  CodeText *text() const { return text_; }
  void showFunction(Address function);
  void setRepresentation(const QString &representation);
  /// The representation the selector shows, loaded or still to load.
  QString representation() const;
  /// The view stays on its function instead of following the disassembly.
  bool locked() const;
  /// Window title of a representation, such as "Pseudocode".
  static QString titleOf(const QString &representation);

signals:
  void representationChanged(const QString &representation);

private:
  void updateStatus();
  Session &session_;
  QComboBox *selector_;
  QToolButton *fold_, *lock_;
  QLabel *status_;
  CodeText *text_;
};

} // namespace neverd::gui
