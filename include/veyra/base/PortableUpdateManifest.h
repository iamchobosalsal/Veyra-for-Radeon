#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace veyra::portable {

inline constexpr qint64 maxManifestBytes = 64 * 1024;
inline constexpr qint64 maxArchiveBytes = 3ll << 30;
inline constexpr qint64 maxExpandedBytes = 8ll << 30;

struct UpdatePackage {
    QString version;
    QString flavor;
    QString asset;
    QString notes;
    QByteArray digest;
    qint64 archiveBytes = 0;
    qint64 expandedBytes = 0;
};

QByteArray updatePublicKey();
bool isRadeonFork();
// An unconfigured fork has no update endpoint. Installing additionally requires
// an explicit build-time opt-in and a fork release verification key.
QString updateRepository();
bool automaticUpdateChecksEnabled();
bool portableUpdatesEnabled();
QString manifestAssetName();
QUrl releasePageUrl(const QString& tag);
QString releaseVersion(const QString& displayVersion);
enum class DownloadRoute { GitHub, GhProxy, GhFast };
std::optional<DownloadRoute> parseDownloadRoute(const QString& name);
QUrl releaseApiUrl(DownloadRoute route = DownloadRoute::GitHub);
QUrl assetUrl(const QString& version, const QString& asset, DownloadRoute route = DownloadRoute::GitHub);
bool allowedRedirect(const QUrl& url);
QString packageAssetName(const QString& version, const QString& flavor);
std::optional<UpdatePackage> readUpdateManifest(
    const QByteArray& envelope, const QString& flavor, const QString& installed,
    const QString& selectedVersion, const QByteArray& publicKey);

} // namespace veyra::portable
