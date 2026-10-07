#include "JumpDialog.h"

#include "Address.h"
#include "Icons.h"
#include "Session.h"
#include "Theme.h"

#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
constexpr char HistoryKey[] = "jump/history";
constexpr int MaxHistory = 25;
constexpr int MaxMatches = 200;
constexpr int SearchDebounceMs = 90;
constexpr int AddressRole = Qt::UserRole;
} // namespace

void JumpDialog::remember(const QString &text) {
  if (text.trimmed().isEmpty())
    return;
  QSettings settings;
  auto history = settings.value(HistoryKey).toStringList();
  history.removeAll(text);
  history.prepend(text);
  while (history.size() > MaxHistory)
    history.removeLast();
  settings.setValue(HistoryKey, history);
}

JumpDialog::JumpDialog(Session &session, QWidget *parent)
    : QDialog(parent), session_(session), input_(new QLineEdit(this)),
      matches_(new QListWidget(this)), hint_(new QLabel(this)) {
  setWindowTitle(tr("Jump anywhere"));
  setWindowIcon(icon(QStringLiteral("jump")));
  resize(620, 420);
  auto *layout = new QVBoxLayout(this);
  input_->setPlaceholderText(
      tr("Address, name or expression (0x401000, main, sub_401000+10)"));
  input_->setFont(Theme::instance().codeFont());
  input_->installEventFilter(this);
  layout->addWidget(input_);
  matches_->setFont(Theme::instance().codeFont());
  matches_->setUniformItemSizes(true);
  layout->addWidget(matches_, 1);
  layout->addWidget(hint_);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &JumpDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(matches_, &QListWidget::itemActivated, this, [this] { accept(); });
  debounce_.setSingleShot(true);
  debounce_.setInterval(SearchDebounceMs);
  connect(&debounce_, &QTimer::timeout, this, &JumpDialog::search);
  connect(input_, &QLineEdit::textChanged, this, [this] {
    matches_->clearSelection();
    debounce_.start();
  });
  // Recent jumps while nothing is typed.
  for (const auto &text : QSettings().value(HistoryKey).toStringList()) {
    auto *item =
        new QListWidgetItem(icon(QStringLiteral("back")), text, matches_);
    item->setData(AddressRole, text);
  }
  hint_->setText(tr("Enter jumps to the expression; arrows choose a match."));
}

void JumpDialog::search() {
  const QString text = input_->text().trimmed();
  const quint64 serial = ++serial_;
  if (text.isEmpty() || !session_.loaded())
    return;
  session_.read(
      QStringLiteral("names"),
      {{"filter", text}, {"limit", MaxMatches}, {"sort", "name"}}, this,
      [this, serial, text](const QJsonObject &payload) {
        if (serial != serial_)
          return;
        matches_->clear();
        const int digits = session_.bitness() == 64 ? 16 : 8;
        for (const auto &value : payload.value("items").toArray()) {
          const auto row = value.toObject();
          const auto address = addressValue(row.value("address"));
          if (!address)
            continue;
          const auto kind = row.value("kind").toString();
          const QString iconName = kind == QLatin1String("import")   ? "imports"
                                   : kind == QLatin1String("string") ? "strings"
                                   : kind == QLatin1String("data")
                                       ? "data_item"
                                       : "functions";
          auto *item = new QListWidgetItem(
              icon(iconName),
              QStringLiteral("%1   %2").arg(row.value("name").toString(),
                                            displayAddress(*address, digits)),
              matches_);
          item->setData(AddressRole, hexAddress(*address));
        }
        hint_->setText(
            tr("%n matches", "", int(payload.value("total").toInt())));
      },
      [this](const QString &, const QString &message) {
        hint_->setText(message);
      });
}

bool JumpDialog::eventFilter(QObject *object, QEvent *event) {
  if (object == input_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
      const int count = matches_->count();
      if (!count)
        return true;
      int row = matches_->currentRow();
      row = key->key() == Qt::Key_Down ? std::min(count - 1, row + 1)
                                       : std::max(-1, row - 1);
      matches_->setCurrentRow(row);
      return true;
    }
  }
  return QDialog::eventFilter(object, event);
}

void JumpDialog::accept() {
  const auto *item = matches_->currentItem();
  if (item && item->isSelected())
    result_ = item->data(AddressRole).toString();
  else
    result_ = input_->text().trimmed();
  if (result_.isEmpty())
    return;
  remember(input_->text().trimmed().isEmpty() ? result_
                                              : input_->text().trimmed());
  QDialog::accept();
}

} // namespace neverd::gui
