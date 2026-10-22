//===- DeepLink.h - A link to a place in a program -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A deep link: a URL that names a place in a program, which a teammate's
/// NeverD, another window or another program opens there, as IDA's ida://
/// links do.  `neverd://<source>/<input>/<resource>?<params>` takes IDA's
/// form, its resources, views and parameters, and NeverD follows IDA's own
/// links too.  Beyond IDA's, a link may name the input by a prefix of its
/// SHA-256 (`sha256=`), which a renamed or moved file keeps and another
/// build does not, and an address may show in the graph or at its line of
/// the pseudocode.  Copy link writes a link, Jump to link and the instance
/// server (InstanceServer) follow one.
//===----------------------------------------------------------------------===//

#pragma once

#include "Address.h"

#include <QList>
#include <QString>
#include <optional>

namespace neverd::gui {

/// What a link names, in IDA's words.
enum class LinkResource {
  Functions,
  Addresses,
  Strings,
  Segments,
  Names,
  Imports,
  Exports,
  Types,
  Bookmarks
};

/// Where a link shows its place.
enum class LinkView {
  Disasm,
  Graph,
  Pseudocode,
  Hexdump,
  Functions,
  Segments,
  Names,
  Imports,
  Exports,
  Strings,
  Types,
  Bookmarks
};

struct DeepLink {
  /// "neverd", or "ida" for a link IDA made.
  QString scheme = QStringLiteral("neverd");
  /// A named source of programs (IDA's hcli sources), empty for this
  /// machine.
  QString source;
  /// The input's file name (IDA's: its database's), empty for whichever
  /// program is open.
  QString input;
  LinkResource resource = LinkResource::Addresses;
  /// The address, relative to the image base (`rva`, as a link is best
  /// written) or as it is (`ea`); a type is named instead.
  std::optional<Address> rva, ea;
  QString name;
  /// The view the link asks for; the resource's own when none.
  std::optional<LinkView> view;
  /// A prefix of the input's SHA-256 in lowercase hexadecimal, 8 to 64
  /// digits: the input the link was made in, whatever its name now.
  QString sha256;

  LinkView shownView() const;
  /// The address the link names in an image based at \p base.
  std::optional<Address> address(Address base) const;
  /// Whether \p sha256, a whole SHA-256, is the input the link names.
  bool namesHash(const QString &sha256) const;
};

/// \p text as a link, or why it is none: a malformed URL, a resource,
/// view or parameter the form does not have, or a view the resource is
/// not shown in.
struct ParsedLink {
  std::optional<DeepLink> link;
  QString error;
};
ParsedLink parseDeepLink(const QString &text);

/// The URL of \p link, relative (`neverd:///input/...`) without a source.
QString formatDeepLink(const DeepLink &link);

/// Whether \p text is written as a link, well formed or not.
bool looksLikeDeepLink(const QString &text);

/// The views \p resource shows in, its own first.
QList<LinkView> linkViews(LinkResource resource);
QString linkResourceName(LinkResource resource);
QString linkViewName(LinkView view);

/// Whether a link's input name \p linkInput names the file \p fileName: the
/// same name, or IDA's database of it (`name.i64`, `name.idb`, or the name
/// with its extension replaced by one of those).
bool linkNamesInput(const QString &linkInput, const QString &fileName);

} // namespace neverd::gui
