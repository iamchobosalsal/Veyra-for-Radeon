#include "veyra/base/PortableLayout.h"
#include "veyra/base/PortableUpdateManifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <openssl/evp.h>

using namespace veyra::portable;

namespace {
// A deterministic test-only signing key, unrelated to any release key. Signing
// well-formed foreign manifests proves rejection is by channel identity rather
// than merely rejecting malformed JSON or an invalid signature.
const QByteArray seed = QByteArray::fromHex(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");

struct SignedManifest {
    QByteArray envelope;
    QByteArray publicKey;
};

SignedManifest sign(const QJsonObject& object) {
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(seed.constData()), seed.size()), EVP_PKEY_free);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    SignedManifest result;
    result.publicKey.resize(32);
    size_t publicBytes = 32, signatureBytes = 64;
    QByteArray signature(64, '\0');
    if (!key || !context
        || EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(result.publicKey.data()), &publicBytes) != 1
        || publicBytes != 32
        || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1
        || EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &signatureBytes,
            reinterpret_cast<const unsigned char*>(payload.constData()), payload.size()) != 1
        || signatureBytes != 64) qFatal("Unable to sign test manifest");
    result.envelope = QJsonDocument(QJsonObject{
        {"payload", QString::fromLatin1(payload.toBase64())},
        {"signature", QString::fromLatin1(signature.toBase64())}}).toJson(QJsonDocument::Compact);
    return result;
}

QJsonObject manifest() {
    return QJsonObject{
        {"schema", 1}, {"product", isRadeonFork() ? "Veyra-Radeon-portable" : "Veyra-portable"},
        {"arch", "windows-x64"}, {"channel", isRadeonFork() ? "radeon-stable" : "stable"},
        {"repository", updateRepository()}, {"version", "1.0.1"},
        {"packages", QJsonArray{QJsonObject{
            {"flavor", "AMD"}, {"asset", packageAssetName("1.0.1", "AMD")},
            {"sha256", QString(64, u'a')}, {"size", 128}, {"expandedSize", 256}}}}};
}

bool accepts(const QJsonObject& object, const QString& flavor = QStringLiteral("AMD")) {
    const auto signedManifest = sign(object);
    return readUpdateManifest(signedManifest.envelope, flavor, "1.0.0", "1.0.1", signedManifest.publicKey).has_value();
}
}

class PortableUpdatePolicyTests : public QObject {
    Q_OBJECT
private slots:
    void packageRootIdentity() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const std::filesystem::path root(directory.path().toStdWString());
        const auto writeMarker = [&](const QByteArray& contents) {
            QFile marker(directory.filePath(QStringLiteral("portable-package.txt")));
            return marker.open(QIODevice::WriteOnly | QIODevice::Truncate) && marker.write(contents) == contents.size();
        };
        QVERIFY(writeMarker("VEYRA_PORTABLE_V1\nAMD\n"));
        QCOMPARE(packageFlavor(root), isRadeonFork() ? std::string{} : std::string{"AMD"});
        QVERIFY(writeMarker("VEYRA_PORTABLE_V1\nNVIDIA\n"));
        QCOMPARE(packageFlavor(root), isRadeonFork() ? std::string{} : std::string{"NVIDIA"});
        QVERIFY(writeMarker("VEYRA_RADEON_PORTABLE_V1\nAMD\n"));
        QCOMPARE(packageFlavor(root), isRadeonFork() ? std::string{"AMD"} : std::string{});
        QVERIFY(writeMarker("VEYRA_RADEON_PORTABLE_V1\nNVIDIA\n"));
        QVERIFY(packageFlavor(root).empty());
    }

    void routingAndDisabledDownloads() {
        if (!isRadeonFork()) {
            QCOMPARE(updateRepository(), QStringLiteral("Likely7/Veyra-NRVideo"));
            QCOMPARE(manifestAssetName(), QStringLiteral("Veyra-portable-update-v1.json"));
            QVERIFY(automaticUpdateChecksEnabled());
            return;
        }
        QVERIFY(!automaticUpdateChecksEnabled());
        QVERIFY(updateRepository().compare(QStringLiteral("Likely7/Veyra-NRVideo"), Qt::CaseInsensitive) != 0);
        QCOMPARE(manifestAssetName(), QStringLiteral("Veyra-Radeon-portable-update-v1.json"));
        QCOMPARE(packageAssetName("1.0.1", "AMD"), QStringLiteral("Veyra-Radeon-1.0.1-AMD-win64-portable.zip"));
        QVERIFY(packageAssetName("1.0.1", "NVIDIA").isEmpty());
        for (auto route : {DownloadRoute::GitHub, DownloadRoute::GhProxy, DownloadRoute::GhFast}) {
            const auto api = releaseApiUrl(route);
            const auto asset = assetUrl("1.0.1", manifestAssetName(), route);
            if (updateRepository().isEmpty()) {
                QVERIFY(api.isEmpty());
                QVERIFY(releasePageUrl("v1.0.1").isEmpty());
            } else {
                QVERIFY(api.toString().contains(updateRepository()));
                QVERIFY(!api.toString().contains("Likely7/Veyra-NRVideo", Qt::CaseInsensitive));
            }
            if (!portableUpdatesEnabled()) QVERIFY(asset.isEmpty());
            else QVERIFY(asset.toString().contains(updateRepository()));
        }
    }

    void signedChannelIsolation() {
        const auto valid = manifest();
        // Also exercises the default disabled policy with a correctly signed,
        // otherwise valid pending manifest: it must never reach activation.
        QCOMPARE(accepts(valid), portableUpdatesEnabled());
        if (!portableUpdatesEnabled()) return;

        auto foreign = valid;
        foreign["product"] = isRadeonFork() ? "Veyra-portable" : "Veyra-Radeon-portable";
        QVERIFY(!accepts(foreign));
        foreign = valid;
        foreign["channel"] = isRadeonFork() ? "stable" : "radeon-stable";
        QVERIFY(!accepts(foreign));
        if (isRadeonFork()) {
            foreign = valid;
            foreign["repository"] = "Likely7/Veyra-NRVideo";
            QVERIFY(!accepts(foreign));
            foreign = valid;
            foreign.remove("repository");
            QVERIFY(!accepts(foreign));
            foreign = valid;
            auto packages = foreign["packages"].toArray();
            auto row = packages[0].toObject();
            row["asset"] = "Veyra-1.0.1-AMD-win64-portable.zip";
            packages[0] = row;
            foreign["packages"] = packages;
            QVERIFY(!accepts(foreign));
            QVERIFY(!accepts(valid, "NVIDIA"));
        }
    }

    void tamperingAndVersionChecks() {
        if (!portableUpdatesEnabled()) QSKIP("Install policy is disabled before signature parsing");
        auto signedManifest = sign(manifest());
        signedManifest.publicKey[0] = char(signedManifest.publicKey[0] ^ 1);
        QVERIFY(!readUpdateManifest(signedManifest.envelope, "AMD", "1.0.0", "1.0.1", signedManifest.publicKey));
        signedManifest = sign(manifest());
        QVERIFY(!readUpdateManifest(signedManifest.envelope, "AMD", "1.0.1", "1.0.1", signedManifest.publicKey));
        QVERIFY(!readUpdateManifest(signedManifest.envelope, "AMD", "1.0.0", "1.0.2", signedManifest.publicKey));
    }
};

QTEST_GUILESS_MAIN(PortableUpdatePolicyTests)
#include "PortableUpdatePolicyTests.moc"
