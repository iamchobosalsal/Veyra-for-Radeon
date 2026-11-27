#include "veyra/base/PortableUpdateManifest.h"
#include "veyra/base/PortableLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <memory>
#include <openssl/evp.h>

#ifndef VEYRA_UPDATE_REPOSITORY
#define VEYRA_UPDATE_REPOSITORY ""
#endif
#ifndef VEYRA_UPDATE_INSTALL_ENABLED
#define VEYRA_UPDATE_INSTALL_ENABLED 0
#endif

namespace veyra::portable {
namespace {

bool verify(const QByteArray& payload, const QByteArray& signature, const QByteArray& key) {
    if (key.size() != 32 || signature.size() != 64 || payload.isEmpty()) return false;
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()), key.size()), EVP_PKEY_free);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return publicKey && context
        && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, publicKey.get()) == 1
        && EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.constData()),
            signature.size(), reinterpret_cast<const unsigned char*>(payload.constData()), payload.size()) == 1;
}

QString origin() {
#ifdef VEYRA_UPDATE_TEST_PORT
    return QStringLiteral("http://127.0.0.1:") + QString::number(VEYRA_UPDATE_TEST_PORT);
#else
    return QStringLiteral("https://github.com");
#endif
}

QUrl routedUrl(const QString& official, DownloadRoute route) {
    if (route == DownloadRoute::GitHub) return QUrl(official);
#ifdef VEYRA_UPDATE_TEST_PORT
    // Both routes stay on the same test server, with distinct paths.
    const auto base = origin();
    const auto prefix = route == DownloadRoute::GhProxy ? QStringLiteral("/mirror/gh-proxy")
                                                       : QStringLiteral("/mirror/ghfast");
    return QUrl(base + prefix + official.sliced(base.size()));
#else
    const auto base = route == DownloadRoute::GhProxy ? QStringLiteral("https://gh-proxy.com/")
                                                     : QStringLiteral("https://ghfast.top/");
    return QUrl(base + official);
#endif
}

} // namespace

QByteArray updatePublicKey() { return QByteArray::fromHex(QByteArrayLiteral(VEYRA_UPDATE_PUBLIC_KEY)); }

bool isRadeonFork() {
#if defined(VEYRA_RADEON_FORK) && VEYRA_RADEON_FORK
    return true;
#else
    return false;
#endif
}

QString updateRepository() {
    if (!isRadeonFork()) return QStringLiteral("Likely7/Veyra-NRVideo");
    const auto repository = QString::fromLatin1(VEYRA_UPDATE_REPOSITORY);
    static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})/[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}\\z"));
    if (!valid.match(repository).hasMatch()
        || repository.compare(QStringLiteral("Likely7/Veyra-NRVideo"), Qt::CaseInsensitive) == 0)
        return {};
    return repository;
}

bool automaticUpdateChecksEnabled() { return !isRadeonFork(); }

bool portableUpdatesEnabled() {
    // Preserve the stock channel's behavior. Its existing constructor/manifest
    // checks still require a valid key before offering or accepting an update.
    if (!isRadeonFork()) return true;
    return VEYRA_UPDATE_INSTALL_ENABLED != 0 && !updateRepository().isEmpty()
        && updatePublicKey().size() == 32;
}

QString manifestAssetName() {
    return isRadeonFork() ? QStringLiteral("Veyra-Radeon-portable-update-v1.json")
                          : QStringLiteral("Veyra-portable-update-v1.json");
}

QUrl releasePageUrl(const QString& tag) {
    const auto repository = updateRepository();
    if (repository.isEmpty()) return {};
    return QUrl(QStringLiteral("https://github.com/") + repository + QStringLiteral("/releases/tag/") + tag);
}

QString releaseVersion(const QString& displayVersion) {
    const auto version = displayVersion.section(u'-', 0, 0);
    return parseVersion(version.toStdString()) ? version : QString{};
}

std::optional<DownloadRoute> parseDownloadRoute(const QString& name) {
    if (name == QLatin1String("github")) return DownloadRoute::GitHub;
    if (name == QLatin1String("gh-proxy")) return DownloadRoute::GhProxy;
    if (name == QLatin1String("ghfast")) return DownloadRoute::GhFast;
    return std::nullopt;
}

QUrl releaseApiUrl(DownloadRoute route) {
    const auto repository = updateRepository();
    if (repository.isEmpty()) return {};
    // GHFast only serves Release assets. Use the GitHub API on this route.
    if (route == DownloadRoute::GhFast) route = DownloadRoute::GitHub;
#ifdef VEYRA_UPDATE_TEST_PORT
    return routedUrl(origin() + QStringLiteral("/repos/") + repository + QStringLiteral("/releases/latest"), route);
#else
    return routedUrl(QStringLiteral("https://api.github.com/repos/") + repository + QStringLiteral("/releases/latest"), route);
#endif
}

QUrl assetUrl(const QString& version, const QString& asset, DownloadRoute route) {
    const auto repository = updateRepository();
    if (repository.isEmpty() || !portableUpdatesEnabled()) return {};
    return routedUrl(origin() + u'/' + repository + QStringLiteral("/releases/download/v")
        + version + u'/' + asset, route);
}

bool allowedRedirect(const QUrl& url) {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
#ifdef VEYRA_UPDATE_TEST_PORT
    return url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1")
        && url.port() == VEYRA_UPDATE_TEST_PORT;
#else
    return url.scheme() == QStringLiteral("https") && url.port(-1) == -1
        && (url.host() == QStringLiteral("release-assets.githubusercontent.com")
            || url.host() == QStringLiteral("objects.githubusercontent.com"));
#endif
}

QString packageAssetName(const QString& version, const QString& flavor) {
    if (isRadeonFork() && flavor != QStringLiteral("AMD")) return {};
    return (isRadeonFork() ? QStringLiteral("Veyra-Radeon-%1-%2-win64-portable.zip")
                           : QStringLiteral("Veyra-%1-%2-win64-portable.zip")).arg(version, flavor);
}

std::optional<UpdatePackage> readUpdateManifest(
    const QByteArray& envelope, const QString& flavor, const QString& installed,
    const QString& selectedVersion, const QByteArray& publicKey) {
    if (!portableUpdatesEnabled() || (isRadeonFork() && flavor != QStringLiteral("AMD"))
        || envelope.isEmpty() || envelope.size() > maxManifestBytes) return {};
    const auto outer = QJsonDocument::fromJson(envelope).object();
    const auto decode = [&outer](const QString& field) {
        return QByteArray::fromBase64(outer.value(field).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    };
    const auto payload = decode(QStringLiteral("payload"));
    if (!verify(payload, decode(QStringLiteral("signature")), publicKey)) return {};

    const auto object = QJsonDocument::fromJson(payload).object();
    const auto version = object.value(QStringLiteral("version")).toString();
    const auto from = parseVersion(installed.toStdString());
    const auto to = parseVersion(version.toStdString());
    if (!from || !to || *to <= *from || (!selectedVersion.isEmpty() && version != selectedVersion)
        || object.value(QStringLiteral("schema")).toInt() != 1
        || object.value(QStringLiteral("product")).toString() != (isRadeonFork()
            ? QStringLiteral("Veyra-Radeon-portable") : QStringLiteral("Veyra-portable"))
        || object.value(QStringLiteral("arch")).toString() != QStringLiteral("windows-x64")
        || object.value(QStringLiteral("channel")).toString() != (isRadeonFork()
            ? QStringLiteral("radeon-stable") : QStringLiteral("stable"))
        || (isRadeonFork() && object.value(QStringLiteral("repository")).toString() != updateRepository())) return {};

    static const QRegularExpression sha256(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    const auto notes = object.value(QStringLiteral("notes")).toString().left(480);
    std::optional<UpdatePackage> package;
    for (const auto& row : object.value(QStringLiteral("packages")).toArray()) {
        const auto item = row.toObject();
        if (item.value(QStringLiteral("flavor")).toString() != flavor) continue;
        const auto asset = item.value(QStringLiteral("asset")).toString();
        const auto hash = item.value(QStringLiteral("sha256")).toString();
        const auto size = item.value(QStringLiteral("size")).toInteger();
        const auto expanded = item.value(QStringLiteral("expandedSize")).toInteger();
        if (package || (flavor != QStringLiteral("NVIDIA") && flavor != QStringLiteral("AMD"))
            || asset != packageAssetName(version, flavor) || !sha256.match(hash).hasMatch()
            || size <= 0 || size > maxArchiveBytes || expanded <= 0 || expanded > maxExpandedBytes) return {};
        package = UpdatePackage{version, flavor, asset, notes, QByteArray::fromHex(hash.toLatin1()), size, expanded};
    }
    return package;
}

} // namespace veyra::portable
