#include "effects/effectsmanager.h"

#include <QDir>
#include <QMetaType>
#include <algorithm>

#include "effects/backends/builtin/builtinbackend.h"
#include "effects/backends/builtin/filtereffect.h"
#include "effects/backends/lv2/lv2backend.h"
#include "effects/effectslot.h"
#include "effects/effectsmessenger.h"
#include "effects/presets/effectchainpreset.h"
#include "effects/presets/effectxmlelements.h"
#include "util/assert.h"

namespace {
const QString kEffectDefaultsDirectory = "/effects/defaults";
const QString kStandardEffectRackGroup = "[EffectRack1]";
const QString kOutputEffectRackGroup = "[OutputEffectRack]";
const QString kQuickEffectRackGroup = "[QuickEffectRack1]";
const QString kEqualizerEffectRackGroup = "[EqualizerRack1]";
const QString kEffectGroupSeparator = "_";
const QString kGroupClose = "]";
const unsigned int kEffectMessagePipeFifoSize = 2048;
} // anonymous namespace

EffectsManager::EffectsManager(QObject* pParent,
        UserSettingsPointer pConfig,
        ChannelHandleFactory* pChannelHandleFactory)
        : QObject(pParent),
          m_pChannelHandleFactory(pChannelHandleFactory),
          m_loEqFreq(ConfigKey("[Mixer Profile]", "LoEQFrequency"), 0., 22040),
          m_hiEqFreq(ConfigKey("[Mixer Profile]", "HiEQFrequency"), 0., 22040),
          m_pConfig(pConfig) {
    qRegisterMetaType<EffectChainMixMode>("EffectChainMixMode");

    QPair<EffectsRequestPipe*, EffectsResponsePipe*> requestPipes =
            TwoWayMessagePipe<EffectsRequest*, EffectsResponse>::makeTwoWayMessagePipe(
                    kEffectMessagePipeFifoSize, kEffectMessagePipeFifoSize);
    m_pMessenger = EffectsMessengerPointer(new EffectsMessenger(
            requestPipes.first, requestPipes.second));
    m_pEngineEffectsManager = new EngineEffectsManager(requestPipes.second);

    m_pNumEffectsAvailable = new ControlObject(ConfigKey("[Master]", "num_effectsavailable"));
    m_pNumEffectsAvailable->setReadOnly();

    addEffectsBackend(EffectsBackendPointer(new BuiltInBackend()));
#ifdef __LILV__
    addEffectsBackend(EffectsBackendPointer(new LV2Backend()));
#endif

    EffectManifestPointer filterEffectManifest = getManifest(FilterEffect::getId(),
            EffectBackendType::BuiltIn);
    m_pChainPresetManager = EffectChainPresetManagerPointer(
            new EffectChainPresetManager(pConfig, this, filterEffectManifest));
}

EffectsManager::~EffectsManager() {
    m_pMessenger->startShutdownProcess();

    saveEffectsXml();
    for (const auto pEffectPreset : m_defaultPresets) {
        saveDefaultForEffect(pEffectPreset);
    }

    // The EffectChainSlots must be deleted before the EffectsBackends in case
    // there is an LV2 effect currently loaded.
    // ~LV2GroupState calls lilv_instance_free, which will segfault if called
    // after ~LV2Backend calls lilv_world_free.
    m_equalizerEffectChainSlots.clear();
    m_quickEffectChainSlots.clear();
    m_standardEffectChainSlots.clear();
    m_outputEffectChainSlot.clear();
    m_effectChainSlotsByGroup.clear();
    m_pMessenger->processEffectsResponses();

    m_effectsBackends.clear();

    // delete m_pHiEqFreq;
    // delete m_pLoEqFreq;
    delete m_pNumEffectsAvailable;
}

bool alphabetizeEffectManifests(EffectManifestPointer pManifest1,
        EffectManifestPointer pManifest2) {
    // Sort built-in effects first before external plugins
    int backendNameComparision = static_cast<int>(pManifest1->backendType()) -
            static_cast<int>(pManifest2->backendType());
    int displayNameComparision = QString::localeAwareCompare(
            pManifest1->displayName(), pManifest2->displayName());
    return (backendNameComparision ? (backendNameComparision < 0) : (displayNameComparision < 0));
}

void EffectsManager::addEffectsBackend(EffectsBackendPointer pBackend) {
    VERIFY_OR_DEBUG_ASSERT(pBackend) {
        return;
    }
    m_effectsBackends.insert(pBackend->getType(), pBackend);

    QList<QString> backendEffects = pBackend->getEffectIds();
    for (const QString& effectId : backendEffects) {
        m_availableEffectManifests.append(pBackend->getManifest(effectId));
    }

    m_pNumEffectsAvailable->forceSet(m_availableEffectManifests.size());

    std::sort(m_availableEffectManifests.begin(), m_availableEffectManifests.end(),
          alphabetizeEffectManifests);
}

bool EffectsManager::isAdoptMetaknobValueEnabled() const {
    return m_pConfig->getValue(ConfigKey("[Effects]", "AdoptMetaknobValue"), true);
}

void EffectsManager::registerInputChannel(const ChannelHandleAndGroup& handle_group) {
    VERIFY_OR_DEBUG_ASSERT(!m_registeredInputChannels.contains(handle_group)) {
        return;
    }
    m_registeredInputChannels.insert(handle_group);

    foreach (EffectChainSlotPointer pChainSlot, m_standardEffectChainSlots) {
        pChainSlot->registerInputChannel(handle_group);
    }
}

void EffectsManager::registerOutputChannel(const ChannelHandleAndGroup& handle_group) {
    VERIFY_OR_DEBUG_ASSERT(!m_registeredOutputChannels.contains(handle_group)) {
        return;
    }
    m_registeredOutputChannels.insert(handle_group);
}

void EffectsManager::loadStandardEffect(const int iChainSlotNumber,
        const int iEffectSlotNumber, const EffectManifestPointer pManifest) {
    auto pChainSlot = getStandardEffectChainSlot(iChainSlotNumber);
    if (pChainSlot) {
        loadEffect(pChainSlot, iEffectSlotNumber, pManifest);
    }
}

void EffectsManager::loadOutputEffect(const int iEffectSlotNumber,
    const EffectManifestPointer pManifest) {
    if (m_outputEffectChainSlot) {
        loadEffect(m_outputEffectChainSlot, iEffectSlotNumber, pManifest);
    }
}

void EffectsManager::loadEqualizerEffect(const QString& deckGroup,
        const int iEffectSlotNumber, const EffectManifestPointer pManifest) {
    auto pChainSlot = m_equalizerEffectChainSlots.value(deckGroup);
    VERIFY_OR_DEBUG_ASSERT(pChainSlot) {
        return;
    }
    loadEffect(pChainSlot, iEffectSlotNumber, pManifest);
}

void EffectsManager::loadEffect(EffectChainSlotPointer pChainSlot,
        const int iEffectSlotNumber,
        const EffectManifestPointer pManifest,
        EffectPresetPointer pPreset,
        bool adoptMetaknobFromPreset) {
    if (pPreset == nullptr) {
        pPreset = m_defaultPresets.value(pManifest);
    }
    pChainSlot->loadEffect(
            iEffectSlotNumber,
            pManifest,
            createProcessor(pManifest),
            pPreset,
            adoptMetaknobFromPreset);
}

std::unique_ptr<EffectProcessor> EffectsManager::createProcessor(
        const EffectManifestPointer pManifest) {
    if (!pManifest) {
        // This can be a valid request to unload an effect, so do not DEBUG_ASSERT.
        return std::unique_ptr<EffectProcessor>(nullptr);
    }
    EffectsBackendPointer pBackend = m_effectsBackends.value(pManifest->backendType());
    VERIFY_OR_DEBUG_ASSERT(pBackend) {
        return std::unique_ptr<EffectProcessor>(nullptr);
    }
    return pBackend->createProcessor(pManifest);
}

ParameterMap EffectsManager::getLoadedParameters(int chainNumber, int effectNumber) const {
    return m_standardEffectChainSlots.at(chainNumber)->getEffectSlot(effectNumber)->getLoadedParameters();
}

ParameterMap EffectsManager::getHiddenParameters(int chainNumber, int effectNumber) const {
    return m_standardEffectChainSlots.at(chainNumber)->getEffectSlot(effectNumber)->getHiddenParameters();
}

void EffectsManager::hideParameter(int chainNumber, int effectNumber, EffectParameterPointer pParameter) {
    m_standardEffectChainSlots.at(chainNumber)->getEffectSlot(effectNumber)->hideParameter(pParameter);
}

void EffectsManager::showParameter(int chainNumber, int effectNumber, EffectParameterPointer pParameter) {
    m_standardEffectChainSlots.at(chainNumber)->getEffectSlot(effectNumber)->showParameter(pParameter);
}

// This needs to be in EffectsManager rather than EffectChainSlot because it
// needs access to the EffectsBackends.
void EffectsManager::loadEffectChainPreset(EffectChainSlot* pChainSlot,
        EffectChainPresetPointer pPreset) {
    VERIFY_OR_DEBUG_ASSERT(pChainSlot) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(pPreset) {
        return;
    }
    pChainSlot->slotControlClear(1);

    int effectSlot = 0;
    for (const auto& pEffectPreset : pPreset->effectPresets()) {
        if (pEffectPreset->isEmpty()) {
            pChainSlot->loadEffect(
                    effectSlot,
                    nullptr,
                    nullptr,
                    nullptr,
                    true);
            effectSlot++;
            continue;
        }
        EffectsBackendPointer pBackend = m_effectsBackends.value(pEffectPreset->backendType());
        VERIFY_OR_DEBUG_ASSERT(pBackend) {
            effectSlot++;
            continue;
        }
        EffectManifestPointer pManifest = pBackend->getManifest(pEffectPreset->id());
        pChainSlot->loadEffect(
                effectSlot,
                pManifest,
                createProcessor(pManifest),
                pEffectPreset,
                true);
        effectSlot++;
    }

    pChainSlot->setMixMode(pPreset->mixMode());
    pChainSlot->setSuperParameterDefaultValue(pPreset->superKnob());
    pChainSlot->setSuperParameter(pPreset->superKnob());
    pChainSlot->setPresetName(pPreset->name());
    pChainSlot->setLoadedPresetIndex(m_pChainPresetManager->presetIndex(pPreset));
}

void EffectsManager::loadEffectChainPreset(EffectChainSlot* pChainSlot, const QString& name) {
    VERIFY_OR_DEBUG_ASSERT(pChainSlot != nullptr) {
        return;
    }
    EffectChainPresetPointer pChainPreset = m_pChainPresetManager->getPreset(name);
    VERIFY_OR_DEBUG_ASSERT(pChainPreset != nullptr) {
        return;
    }
    loadEffectChainPreset(pChainSlot, pChainPreset);
}

void EffectsManager::loadPresetToStandardChain(int chainNumber, EffectChainPresetPointer pPreset) {
    loadEffectChainPreset(m_standardEffectChainSlots.at(chainNumber).get(), pPreset);
}

const QList<EffectManifestPointer> EffectsManager::getAvailableEffectManifestsFiltered(
        EffectManifestFilterFnc filter) const {
    if (filter == nullptr) {
        return m_availableEffectManifests;
    }

    QList<EffectManifestPointer> list;
    for (const auto& pManifest : m_availableEffectManifests) {
        if (filter(pManifest.data())) {
            list.append(pManifest);
        }
    }
    return list;
}

QString EffectsManager::getNextEffectId(const QString& effectId) {
    if (m_visibleEffectManifests.isEmpty()) {
        return QString();
    }
    if (effectId.isNull()) {
        return m_visibleEffectManifests.first()->id();
    }

    int index;
    for (index = 0; index < m_visibleEffectManifests.size(); ++index) {
        if (effectId == m_visibleEffectManifests.at(index)->id()) {
            break;
        }
    }
    if (++index >= m_visibleEffectManifests.size()) {
        index = 0;
    }
    return m_visibleEffectManifests.at(index)->id();
}

QString EffectsManager::getPrevEffectId(const QString& effectId) {
    if (m_visibleEffectManifests.isEmpty()) {
        return QString();
    }
    if (effectId.isNull()) {
        return m_visibleEffectManifests.last()->id();
    }

    int index;
    for (index = 0; index < m_visibleEffectManifests.size(); ++index) {
        if (effectId == m_visibleEffectManifests.at(index)->id()) {
            break;
        }
    }
    if (--index < 0) {
        index = m_visibleEffectManifests.size() - 1;
    }
    return m_visibleEffectManifests.at(index)->id();
}

void EffectsManager::getEffectManifestAndBackend(
        const QString& effectId,
        EffectManifestPointer* ppManifest, EffectsBackend** ppBackend) const {
    for (const auto& pBackend : m_effectsBackends) {
        if (pBackend->canInstantiateEffect(effectId)) {
            *ppManifest = pBackend->getManifest(effectId);
            *ppBackend = pBackend.data();
        }
    }
}

EffectManifestPointer EffectsManager::getManifestFromUniqueId(const QString& uid) const {
    if (kEffectDebugOutput) {
        //qDebug() << "EffectsManager::getManifestFromUniqueId" << uid;
    }
    if (uid.isEmpty()) {
        // Do not DEBUG_ASSERT, this may be a valid request for a nullptr to
        // unload an effect.
        return EffectManifestPointer();
    }
    int delimiterIndex = uid.lastIndexOf(" ");
    EffectBackendType backendType = EffectManifest::backendTypeFromString(
            uid.mid(delimiterIndex+1));
    VERIFY_OR_DEBUG_ASSERT(backendType != EffectBackendType::Unknown) {
        // Mixxx 2.0 - 2.2 did not store the backend type in mixxx.cfg,
        // so this code will be executed once when upgrading to Mixxx 2.3.
        // This debug assertion is safe to ignore in that case. If it is
        // triggered at any later time, there is a bug somewhere.
        // Do not manipulate the string passed to this function, just pass
        // it directly to BuiltInBackend.
        return m_effectsBackends.value(EffectBackendType::BuiltIn)->getManifest(uid);
    }
    return m_effectsBackends.value(backendType)->getManifest(
            uid.mid(-1, delimiterIndex+1));
}

EffectManifestPointer EffectsManager::getManifest(
        const QString& id, EffectBackendType backendType) const {
    return m_effectsBackends.value(backendType)->getManifest(id);
}

void EffectsManager::addStandardEffectChainSlots() {
    for (int i = 0; i < EffectsManager::kNumStandardEffectChains; ++i) {
        VERIFY_OR_DEBUG_ASSERT(!m_effectChainSlotsByGroup.contains(
                StandardEffectChainSlot::formatEffectChainSlotGroup(i))) {
            continue;
        }

        auto pChainSlot = StandardEffectChainSlotPointer(
                new StandardEffectChainSlot(i, this, m_pMessenger));
        connectChainSlotSignals(pChainSlot);

        m_standardEffectChainSlots.append(pChainSlot);
        m_effectChainSlotsByGroup.insert(pChainSlot->group(), pChainSlot);
    }
}

void EffectsManager::addOutputEffectChainSlot() {
    m_outputEffectChainSlot = OutputEffectChainSlotPointer(
            new OutputEffectChainSlot(this, m_pMessenger));
    connectChainSlotSignals(m_outputEffectChainSlot);
    m_effectChainSlotsByGroup.insert(m_outputEffectChainSlot->group(), m_outputEffectChainSlot);
}

EffectChainSlotPointer EffectsManager::getOutputEffectChainSlot() const {
    return m_outputEffectChainSlot;
}

EffectChainSlotPointer EffectsManager::getStandardEffectChainSlot(int unitNumber) const {
    VERIFY_OR_DEBUG_ASSERT(0 <= unitNumber || unitNumber < m_standardEffectChainSlots.size()) {
        return EffectChainSlotPointer();
    }
    return m_standardEffectChainSlots.at(unitNumber);
}

void EffectsManager::addEqualizerEffectChainSlot(const QString& deckGroupName) {
    VERIFY_OR_DEBUG_ASSERT(!m_equalizerEffectChainSlots.contains(
            EqualizerEffectChainSlot::formatEffectChainSlotGroup(deckGroupName))) {
        return;
    }

    auto pChainSlot = EqualizerEffectChainSlotPointer(
            new EqualizerEffectChainSlot(deckGroupName, this, m_pMessenger));
    connectChainSlotSignals(pChainSlot);

    m_equalizerEffectChainSlots.insert(deckGroupName, pChainSlot);
    m_effectChainSlotsByGroup.insert(pChainSlot->group(), pChainSlot);
}

void EffectsManager::addQuickEffectChainSlot(const QString& deckGroupName) {
    VERIFY_OR_DEBUG_ASSERT(!m_quickEffectChainSlots.contains(
            QuickEffectChainSlot::formatEffectChainSlotGroup(deckGroupName))) {
        return;
    }

    auto pChainSlot = QuickEffectChainSlotPointer(
            new QuickEffectChainSlot(deckGroupName, this, m_pMessenger));
    connectChainSlotSignals(pChainSlot);

    m_quickEffectChainSlots.insert(deckGroupName, pChainSlot);
    m_effectChainSlotsByGroup.insert(pChainSlot->group(), pChainSlot);
}

void EffectsManager::connectChainSlotSignals(EffectChainSlotPointer pChainSlot) {
    connect(pChainSlot.get(),
            &EffectChainSlot::loadChainPreset,
            this,
            &EffectsManager::loadChainPresetFromList);
    connect(pChainSlot.get(),
            &EffectChainSlot::selectChainPreset,
            this,
            &EffectsManager::loadChainPresetSelector);
}

void EffectsManager::loadChainPresetFromList(EffectChainSlot* pChainSlot, int listIndex) {
    loadEffectChainPreset(pChainSlot, m_pChainPresetManager->presetAtIndex(listIndex));
}

void EffectsManager::loadChainPresetSelector(EffectChainSlot* pChainSlot, int delta) {
    int listIndex = m_pChainPresetManager->presetIndex(pChainSlot->presetName());
    auto pChainPreset = m_pChainPresetManager->presetAtIndex(listIndex + delta);
    loadEffectChainPreset(pChainSlot, pChainPreset);
}

EffectChainSlotPointer EffectsManager::getEffectChainSlot(
        const QString& group) const {
    return m_effectChainSlotsByGroup.value(group);
}

EffectSlotPointer EffectsManager::getEffectSlot(
        const QString& group) {
    QRegExp intRegEx(".*(\\d+).*");

    QStringList parts = group.split(kEffectGroupSeparator);

    // NOTE(Kshitij) : Assuming the group is valid
    const QString chainGroup = parts.at(0) + kEffectGroupSeparator + parts.at(1) + kGroupClose;
    EffectChainSlotPointer pChainSlot = getEffectChainSlot(chainGroup);
    VERIFY_OR_DEBUG_ASSERT(pChainSlot) {
        return EffectSlotPointer();
    }

    intRegEx.indexIn(parts.at(2));
    EffectSlotPointer pEffectSlot =
            pChainSlot->getEffectSlot(intRegEx.cap(1).toInt() - 1);
    return pEffectSlot;
}

EffectParameterSlotBasePointer EffectsManager::getEffectParameterSlot(
        const EffectParameterType parameterType,
        const ConfigKey& configKey) {
    EffectSlotPointer pEffectSlot =
             getEffectSlot(configKey.group);
    VERIFY_OR_DEBUG_ASSERT(pEffectSlot) {
        return EffectParameterSlotBasePointer();
    }

    QRegExp intRegEx(".*(\\d+).*");
    intRegEx.indexIn(configKey.item);
    EffectParameterSlotBasePointer pParameterSlot = pEffectSlot->getEffectParameterSlot(
            parameterType, intRegEx.cap(1).toInt() - 1);
    return pParameterSlot;
}

void EffectsManager::setEffectVisibility(EffectManifestPointer pManifest, bool visible) {
    if (visible && !m_visibleEffectManifests.contains(pManifest)) {
        auto insertion_point = std::lower_bound(m_visibleEffectManifests.begin(),
                                                m_visibleEffectManifests.end(),
                                                pManifest, alphabetizeEffectManifests);
        m_visibleEffectManifests.insert(insertion_point, pManifest);
        emit visibleEffectsUpdated();
    } else if (!visible) {
        m_visibleEffectManifests.removeOne(pManifest);
        emit visibleEffectsUpdated();
    }
}

bool EffectsManager::getEffectVisibility(EffectManifestPointer pManifest) {
    return m_visibleEffectManifests.contains(pManifest);
}

void EffectsManager::setup() {
    // Add postfader effect chain slots
    addStandardEffectChainSlots();
    addOutputEffectChainSlot();

    loadDefaultEffectPresets();

    readEffectsXml();
}

void EffectsManager::loadDefaultEffectPresets() {
    // Load saved defaults from settings directory
    QString dirPath(m_pConfig->getSettingsPath() + kEffectDefaultsDirectory);
    QDir effectsDefaultsDir(dirPath);
    effectsDefaultsDir.setFilter(QDir::Files | QDir::Readable);
    for (const auto& filePath : effectsDefaultsDir.entryList()) {
        QFile file(dirPath + "/" + filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QDomDocument doc;
        if (!doc.setContent(&file)) {
            file.close();
            continue;
        }
        EffectPresetPointer pEffectPreset(new EffectPreset(doc.documentElement()));
        if (!pEffectPreset->isEmpty()) {
            EffectManifestPointer pManifest = getManifest(pEffectPreset->id(), pEffectPreset->backendType());
            m_defaultPresets.insert(pManifest, pEffectPreset);
        }
        file.close();
    }

    // If no preset was found, generate one from the manifest
    for (const auto pBackend : m_effectsBackends) {
        for (const auto pManifest : pBackend->getManifests()) {
            if (!m_defaultPresets.contains(pManifest)) {
                m_defaultPresets.insert(pManifest,
                        EffectPresetPointer(new EffectPreset(pManifest)));
            }
        }
    }
}

void EffectsManager::saveDefaultForEffect(EffectPresetPointer pEffectPreset) {
    if (pEffectPreset->isEmpty()) {
        return;
    }

    const auto pBackend = m_effectsBackends.value(pEffectPreset->backendType());
    const auto pManifest = pBackend->getManifest(pEffectPreset->id());
    m_defaultPresets.insert(pManifest, pEffectPreset);

    QDomDocument doc(EffectXml::Effect);
    doc.setContent(QString("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"));
    doc.appendChild(pEffectPreset->toXml(&doc));

    QString path(m_pConfig->getSettingsPath() + kEffectDefaultsDirectory);
    QDir effectsDefaultsDir(path);
    if (!effectsDefaultsDir.exists()) {
        effectsDefaultsDir.mkpath(path);
    }

    // The file name does not matter as long as it is unique. The actual id string
    // is safely stored in the UTF8 document, regardless of what the filesystem
    // supports for file names.
    QString fileName = pEffectPreset->id();
    // LV2 ids are URLs
    fileName.replace("/", "-");
    QStringList forbiddenCharacters;
    forbiddenCharacters << "<"
                        << ">"
                        << ":"
                        << "\""
                        << "\'"
                        << "|"
                        << "?"
                        << "*"
                        << "\\";
    for (const auto& character : forbiddenCharacters) {
        fileName.remove(character);
    }
    QFile file(path + "/" + fileName + ".xml");
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        return;
    }
    file.write(doc.toString().toUtf8());
    file.close();
}

void EffectsManager::saveDefaultForEffect(int unitNumber, int effectNumber) {
    auto pSlot = m_standardEffectChainSlots.at(unitNumber)->getEffectSlot(effectNumber);
    EffectPresetPointer pPreset(new EffectPreset(pSlot));
    saveDefaultForEffect(pPreset);
}

const QString EffectsManager::getDisplayNameForEffectPreset(EffectPresetPointer pPreset) {
    QString displayName(tr("None"));
    if (pPreset == nullptr || pPreset->isEmpty()) {
        return displayName;
    }

    bool manifestFound = false;
    for (const auto pManifest : m_availableEffectManifests) {
        if (pManifest->id() == pPreset->id() &&
                pManifest->backendType() == pPreset->backendType()) {
            displayName = pManifest->name();
            manifestFound = true;
            break;
        }
    }
    DEBUG_ASSERT(manifestFound);
    return displayName;
}

void EffectsManager::savePresetFromStandardEffectChain(int chainNumber) {
    StandardEffectChainSlotPointer pStandardChainSlot = m_standardEffectChainSlots.at(chainNumber);
    EffectChainSlot* genericChainSlot = static_cast<EffectChainSlot*>(pStandardChainSlot.get());
    EffectChainPresetPointer pPreset(new EffectChainPreset(genericChainSlot));
    m_pChainPresetManager->savePreset(pPreset);
}

void EffectsManager::readEffectsXml() {
    QStringList deckStrings;
    for (auto it = m_quickEffectChainSlots.begin(); it != m_quickEffectChainSlots.end(); it++) {
        deckStrings << it.key();
    }
    EffectsXmlData data = m_pChainPresetManager->readEffectsXml(deckStrings);

    for (int i = 0; i < data.standardEffectChainPresets.size(); i++) {
        loadEffectChainPreset(
                m_standardEffectChainSlots.value(i).get(), data.standardEffectChainPresets.at(i));
    }

    for (auto it = data.quickEffectChainPresets.begin();
            it != data.quickEffectChainPresets.end();
            it++) {
        loadEffectChainPreset(m_quickEffectChainSlots.value(it.key()).get(), it.value());
    }
}

void EffectsManager::saveEffectsXml() {
    QHash<QString, EffectChainPresetPointer> quickEffectChainPresets;
    for (auto it = m_quickEffectChainSlots.begin(); it != m_quickEffectChainSlots.end(); it++) {
        auto pPreset = EffectChainPresetPointer(new EffectChainPreset(it.value().get()));
        quickEffectChainPresets.insert(it.key(), pPreset);
    }

    QList<EffectChainPresetPointer> standardEffectChainPresets;
    for (const auto& pChainSlot : m_standardEffectChainSlots) {
        auto pPreset = EffectChainPresetPointer(new EffectChainPreset(pChainSlot.get()));
        standardEffectChainPresets.append(pPreset);
    }

    m_pChainPresetManager->saveEffectsXml(EffectsXmlData{
            quickEffectChainPresets, standardEffectChainPresets});
}
