//===- PathfinderView.h - IDA's Pathfinder window ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's Pathfinder: the call paths between waypoints, functions or imports
/// visited in order, one segment between each two, on the graph the
/// functions' direct references make (call_paths).  The search runs once two
/// waypoints are listed and again whenever the waypoints, the exclusions or
/// the options change.  Between two waypoints the list shows the segment:
/// its paths, shortest first, each with the function or import every step
/// reaches and the instruction that refers to it.  Shift+F9 starts a new path
/// at the function under the cursor and Ctrl+Shift+F9 adds it to the path.
/// The project keeps the waypoints and exclusions, this machine's settings
/// the options, as IDA keeps them.
//===----------------------------------------------------------------------===//

#pragma once

#include "Address.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <optional>

class QCheckBox;
class QLabel;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace neverd::gui {

class AddressSpace;
class Session;

class PathfinderView final : public QWidget {
  Q_OBJECT
public:
  PathfinderView(Session &session, const AddressSpace &space,
                 QWidget *parent = nullptr);

  /// The waypoints with each segment between two of them.
  QTreeWidget *waypointList() const { return waypointList_; }
  QTreeWidget *exclusionList() const { return exclusionList_; }
  QVector<Address> waypoints() const { return waypoints_; }
  QVector<Address> exclusions() const { return exclusions_; }

  /// Starts a new path at the function or import at \p address (Shift+F9).
  void startPath(Address address);
  /// Adds the function or import at \p address as the last waypoint
  /// (Ctrl+Shift+F9), as its entry for an address inside a function.
  void addWaypoint(Address address);
  /// Keeps the function or import at \p address out of every path.
  void addExclusion(Address address);
  /// Lets the user choose a function or import to add as a waypoint (Ins),
  /// or to exclude.
  void chooseWaypoint();
  void chooseExclusion();
  /// Removes the current waypoint or exclusion (Del).
  void removeCurrent();
  /// Moves the current waypoint one place earlier or later.
  void moveCurrent(int by);
  /// Removes every waypoint.
  void clearWaypoints();
  /// The selected rows as text, a line each, columns separated by tabs.
  QString selectedText() const;

  /// The waypoints and exclusions, as the project keeps them.
  QJsonObject state() const;
  void restoreState(const QJsonObject &state);

signals:
  /// The user activated a waypoint or a step's function, or the instruction
  /// that refers to it.
  void activated(neverd::gui::Address address);
  /// The user changed the waypoints or the exclusions.
  void changed();

private:
  enum class Role { Waypoint, Exclusion };
  struct Node {
    QString name;
    bool import = false;
  };

  /// Resolves \p address to the function containing it or the import it
  /// holds, then adds that node as \p role.
  void resolve(Address address, Role role, bool restart);
  void add(Address node, const Node &described, Role role, bool restart);
  void choose(Role role);
  void setWaypoints(QVector<Address> waypoints);
  void scheduleSearch();
  void search();
  void found(const QJsonObject &payload);
  void failed(const QString &message);
  void showWaypoints();
  void showExclusions();
  void showStatus();
  void updateButtons();
  void saveOptions() const;
  void contextMenu(const QPoint &point);
  QString label(Address address) const;
  QTreeWidgetItem *segmentItem(const QJsonObject &segment, int index);
  void unload();

  Session &session_;
  const AddressSpace &space_;
  QTreeWidget *waypointList_, *exclusionList_;
  QToolButton *add_, *remove_, *up_, *down_, *clear_, *exclude_;
  QToolButton *exclusionsToggle_, *optionsToggle_;
  QWidget *exclusionsPanel_, *optionsPanel_;
  QCheckBox *dataRefs_, *shortestOnly_;
  QSpinBox *maxDepth_;
  QLabel *status_;
  QVector<Address> waypoints_, exclusions_;
  /// Names and kinds of every node shown, from resolve and call_paths.
  QHash<Address, Node> nodes_;
  /// The last reply's segments, one between each two waypoints, and what
  /// it found; empty while a search runs or none can.
  QJsonArray segments_;
  int unconnected_ = 0;
  bool searching_ = false;
  QString error_;
  QTimer debounce_;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
