/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/system.h"
#include "common/random.h"
#include "common/config-manager.h"
#include "common/serializer.h"
#include "common/memstream.h"

#include "engines/nancy/nancy.h"
#include "engines/nancy/sound.h"
#include "engines/nancy/input.h"
#include "engines/nancy/util.h"
#include "engines/nancy/graphics.h"
#include "engines/nancy/resource.h"

#include "engines/nancy/action/conversation.h"

#include "engines/nancy/state/scene.h"

namespace Nancy {
namespace Action {

ConversationSound::ConversationSound() :
	RenderActionRecord(8),
	_noResponse(g_nancy->getGameType() <= kGameTypeNancy2 ? 10 : 20) {}

ConversationSound::~ConversationSound() {
	if (NancySceneState.getActiveConversation() == this) {
		NancySceneState.setActiveConversation(nullptr);
	}

	NancySceneState.setShouldClearTextbox(true);
	NancySceneState.getTextbox().setVisible(false);
}

void ConversationSound::init() {
	RenderObject::init();
	NancySceneState.setShouldClearTextbox(false);
}

void ConversationSound::readData(Common::SeekableReadStream &stream) {
	Common::Serializer ser(&stream, nullptr);
	ser.setVersion(g_nancy->getGameType());

	if (ser.getVersion() >= kGameTypeNancy2) {
		_sound.readNormal(stream);
	}

	char *rawText = new char[1500];
	ser.syncBytes((byte *)rawText, 1500);
	UI::Textbox::assembleTextLine(rawText, _text, 1500);
	delete[] rawText;

	if (ser.getVersion() <= kGameTypeNancy1) {
		_sound.readNormal(stream);
	}

	_responseGenericSound.readNormal(stream);
	ser.skip(1);
	ser.syncAsByte(_conditionalResponseCharacterID);
	ser.syncAsByte(_goodbyeResponseCharacterID);
	ser.syncAsByte(_defaultNextScene);
	ser.syncAsByte(_popNextScene);
	_sceneChange.readData(stream, ser.getVersion() == kGameTypeVampire);
	ser.skip(0x32, kGameTypeVampire, kGameTypeNancy1);
	ser.skip(2, kGameTypeNancy2);

	uint16 numResponses = 0;
	ser.syncAsUint16LE(numResponses);
	rawText = new char[400];

	_responses.resize(numResponses);
	for (uint i = 0; i < numResponses; ++i) {
		ResponseStruct &response = _responses[i];
		response.conditionFlags.read(stream);
		ser.syncBytes((byte*)rawText, 400);
		UI::Textbox::assembleTextLine(rawText, response.text, 400);
		readFilename(stream, response.soundName);
		ser.skip(1);
		response.sceneChange.readData(stream, ser.getVersion() == kGameTypeVampire);
		ser.syncAsSint16LE(response.flagDesc.label);
		ser.syncAsByte(response.flagDesc.flag);
		ser.skip(0x32, kGameTypeVampire, kGameTypeNancy1);
		ser.skip(2, kGameTypeNancy2);
	}

	delete[] rawText;

	uint16 numSceneBranchStructs = stream.readUint16LE();
	_sceneBranchStructs.resize(numSceneBranchStructs);
	for (uint i = 0; i < numSceneBranchStructs; ++i) {
		_sceneBranchStructs[i].conditions.read(stream);
		_sceneBranchStructs[i].sceneChange.readData(stream, g_nancy->getGameType() == kGameTypeVampire);
		ser.skip(0x32, kGameTypeVampire, kGameTypeNancy1);
		ser.skip(2, kGameTypeNancy2);
	}

	uint16 numFlagsStructs = stream.readUint16LE();
	_flagsStructs.resize(numFlagsStructs);
	for (uint i = 0; i < numFlagsStructs; ++i) {
		FlagsStruct &flagsStruct = _flagsStructs[i];
		flagsStruct.conditions.read(stream);
		flagsStruct.flagToSet.type = stream.readByte();
		flagsStruct.flagToSet.flag.label = stream.readSint16LE();
		flagsStruct.flagToSet.flag.flag = stream.readByte();
	}
}

void ConversationSound::execute() {
	ConversationSound *activeConversation = NancySceneState.getActiveConversation();
	if (activeConversation != this && activeConversation != nullptr) {
		if (	!activeConversation->_isDone ||
				activeConversation->_defaultNextScene == kDefaultNextSceneEnabled ||
				activeConversation->_pickedResponse != -1	) {

			return;
		} else {
			// Chained videos, hide the previous one and start this
			activeConversation->setVisible(false);
			NancySceneState.setActiveConversation(this);
		}
	}

	switch (_state) {
	case kBegin: {
		init();
		g_nancy->_sound->loadSound(_sound);

		if (!ConfMan.getBool("speech_mute") && ConfMan.getBool("character_speech")) {
			g_nancy->_sound->playSound(_sound);
		}

		// Remove held item and re-add it to inventory
		int heldItem = NancySceneState.getHeldItem();
		if (heldItem != -1) {
			NancySceneState.addItemToInventory(heldItem);
			NancySceneState.setHeldItem(-1);
		}

		// Move the mouse to the default position defined in CURS
		const Common::Point initialMousePos = g_nancy->_cursorManager->getPrimaryVideoInitialPos();
		const Common::Point cursorHotspot = g_nancy->_cursorManager->getCurrentCursorHotspot();
		Common::Point adjustedMousePos = g_nancy->_input->getInput().mousePos;
		adjustedMousePos.x -= cursorHotspot.x;
		adjustedMousePos.y -= cursorHotspot.y - 1;
		if (g_nancy->_cursorManager->getPrimaryVideoInactiveZone().bottom > adjustedMousePos.y) {
			g_system->warpMouse(initialMousePos.x + cursorHotspot.x, initialMousePos.y + cursorHotspot.y);
			g_nancy->_cursorManager->setCursorType(CursorManager::kNormalArrow);
		}

		_state = kRun;
		NancySceneState.setActiveConversation(this);

		// Do not draw first frame since video won't be loaded yet
		g_nancy->_graphicsManager->suppressNextDraw();

		// Do not fall through to give the execution one loop for event flag changes
		// This fixes TVD scene 750
		break;
	}
	case kRun:
		if (!_hasDrawnTextbox) {
			_hasDrawnTextbox = true;
			NancySceneState.getTextbox().clear();

			if (ConfMan.getBool("subtitles")) {
				NancySceneState.getTextbox().addTextLine(_text);
			}

			// Add responses when conditions have been satisfied
			if (_conditionalResponseCharacterID != _noResponse) {
				addConditionalDialogue();
			}

			if (_goodbyeResponseCharacterID != _noResponse) {
				addGoodbye();
			}

			for (uint i = 0; i < _responses.size(); ++i) {
				auto &res = _responses[i];

				if (res.conditionFlags.isSatisfied()) {
					NancySceneState.getTextbox().addTextLine(res.text);
					res.isOnScreen = true;
				}
			}
		}

		if (!g_nancy->_sound->isSoundPlaying(_sound) && isVideoDonePlaying()) {
			g_nancy->_sound->stopSound(_sound);

			bool hasResponses = false;
			for (auto &res : _responses) {
				if (res.isOnScreen) {
					hasResponses = true;
					break;
				}
			}

			if (!hasResponses) {
				// NPC has finished talking with no responses available, auto-advance to next scene
				_state = kActionTrigger;
			} else {
				// NPC has finished talking, we have responses
				for (uint i = 0; i < 30; ++i) {
					if (NancySceneState.getLogicCondition(i, g_nancy->_true)) {
						int pickedOnScreenResponse = _pickedResponse = i;

						// Adjust to account for hidden responses
						for (uint j = 0; j < _responses.size(); ++j) {
							if (!_responses[j].isOnScreen) {
								++_pickedResponse;
							}

							if ((int)j == pickedOnScreenResponse) {
								break;
							}
						}

						break;
					}
				}

				if (_pickedResponse != -1) {
					// Player has picked response, play sound file and change state
					_responseGenericSound.name = _responses[_pickedResponse].soundName;
					g_nancy->_sound->loadSound(_responseGenericSound);

					if (!ConfMan.getBool("speech_mute") && ConfMan.getBool("player_speech")) {
						g_nancy->_sound->playSound(_responseGenericSound);
					}

					_state = kActionTrigger;
				}
			}
		}
		break;
	case kActionTrigger:
		// process flags structs
		for (auto &flags : _flagsStructs) {
			if (flags.conditions.isSatisfied()) {
				flags.flagToSet.set();
			}
		}

		if (_pickedResponse != -1) {
			// Set response's event flag, if any
			NancySceneState.setEventFlag(_responses[_pickedResponse].flagDesc);
		}

		if (!g_nancy->_sound->isSoundPlaying(_responseGenericSound)) {
			g_nancy->_sound->stopSound(_responseGenericSound);

			if (_pickedResponse != -1) {
				NancySceneState.changeScene(_responses[_pickedResponse].sceneChange);
			} else {
				for (uint i = 0; i < _sceneBranchStructs.size(); ++i) {
					if (_sceneBranchStructs[i].conditions.isSatisfied()) {
						NancySceneState.changeScene(_sceneBranchStructs[i].sceneChange);
						break;
					}
				}

				if (_defaultNextScene == kDefaultNextSceneEnabled) {
					NancySceneState.changeScene(_sceneChange);
				} else if (_popNextScene == kPopNextScene) {
					// Exit dialogue
					NancySceneState.popScene();
				}
			}

			finishExecution();
		}

		break;
	}
}

void ConversationSound::addConditionalDialogue() {
	for (const auto &res : g_nancy->getStaticData().conditionalDialogue[_conditionalResponseCharacterID]) {
		bool isSatisfied = true;

		for (const auto &cond : res.flagConditions) {
			if (!NancySceneState.getEventFlag(cond.label, cond.flag)) {
				isSatisfied = false;
				break;
			}
		}

		for (const auto &cond : res.inventoryConditions) {
			if (NancySceneState.hasItem(cond.label) != cond.flag) {
				isSatisfied = false;
				break;
			}
		}

		if (isSatisfied) {
			_responses.push_back(ResponseStruct());
			ResponseStruct &newResponse = _responses.back();
			newResponse.soundName = res.soundID;
			newResponse.text = g_nancy->getStaticData().conditionalDialogueTexts[res.textID];
			newResponse.sceneChange.sceneID = res.sceneID;
			newResponse.sceneChange.continueSceneSound = kContinueSceneSound;
		}
	}
}

void ConversationSound::addGoodbye() {
	auto &res = g_nancy->getStaticData().goodbyes[_goodbyeResponseCharacterID];
	_responses.push_back(ResponseStruct());
	ResponseStruct &newResponse = _responses.back();
	newResponse.soundName = res.soundID;
	newResponse.text = g_nancy->getStaticData().goodbyeTexts[_goodbyeResponseCharacterID];

	// Evaluate conditions to pick from the collection of replies
	uint sceneChangeID = 0;
	for (uint i = 0; i < res.sceneChanges.size(); ++i) {
		const GoodbyeSceneChange &sc = res.sceneChanges[i];
		if (sc.flagConditions.size() == 0) {
			// No conditions, default choice
			sceneChangeID = i;
			break;
		} else {
			bool isSatisfied = true;

			for (const auto &cond : sc.flagConditions) {
				if (!NancySceneState.getEventFlag(cond.label, cond.flag)) {
					isSatisfied = false;
					break;
				}
			}

			if (isSatisfied) {
				sceneChangeID = i;
				break;
			}
		}
	}

	const GoodbyeSceneChange &sceneChange = res.sceneChanges[sceneChangeID];

	// The reply from the character is picked randomly
	newResponse.sceneChange.sceneID = sceneChange.sceneIDs[g_nancy->_randomSource->getRandomNumber(sceneChange.sceneIDs.size() - 1)];

	// Set an event flag if applicable
	NancySceneState.setEventFlag(sceneChange.flagToSet);

	newResponse.sceneChange.continueSceneSound = kContinueSceneSound;
}

void ConversationSound::ConversationFlag::read(Common::SeekableReadStream &stream) {
	type = stream.readByte();
	flag.label = stream.readSint16LE();
	flag.flag = stream.readByte();
	orFlag = stream.readByte();
}

bool ConversationSound::ConversationFlag::isSatisfied() const {
	switch (type) {
	case kFlagEvent:
		return NancySceneState.getEventFlag(flag);
	case kFlagInventory:
		return NancySceneState.hasItem(flag.label) == flag.flag;
	default:
		return false;
	}
}

void ConversationSound::ConversationFlag::set() const {
	switch (type) {
	case kFlagEvent:
		NancySceneState.setEventFlag(flag);
		break;
	case kFlagInventory:
		if (flag.flag == g_nancy->_true) {
			NancySceneState.addItemToInventory(flag.label);
		} else {
			NancySceneState.removeItemFromInventory(flag.label);
		}

		break;
	default:
		break;
	}
}

void ConversationSound::ConversationFlags::read(Common::SeekableReadStream &stream) {
	uint16 numFlags = stream.readUint16LE();

	conditionFlags.resize(numFlags);
	for (uint i = 0; i < numFlags; ++i) {
		conditionFlags[i].read(stream);
	}
}

bool ConversationSound::ConversationFlags::isSatisfied() const {
	Common::Array<bool> conditionsMet(conditionFlags.size(), false);

	for (uint i = 0; i < conditionFlags.size(); ++i) {
		if (conditionFlags[i].isSatisfied()) {
			conditionsMet[i] = true;
		}

		if (conditionFlags[i].orFlag && i < conditionFlags.size() - 1) {
			if (conditionsMet[i] == true) {
				conditionsMet[i + 1] = true;
				++i;
			} else if (conditionFlags[i + 1].isSatisfied()) {
				conditionsMet[i] = true;
				conditionsMet[i + 1] = true;
				++i;
			}
		}
	}

	for (uint i = 0; i < conditionsMet.size(); ++i) {
		if (conditionsMet[i] == false) {
			return false;
		}
	}

	return true;
}

void ConversationVideo::init() {
	if (!_decoder.loadFile(_videoName + ".avf")) {
		error("Couldn't load video file %s", _videoName.c_str());
	}

	_decoder.seekToFrame(_firstFrame);

	if (!_paletteName.empty()) {
		GraphicsManager::loadSurfacePalette(_drawSurface, _paletteName);
		setTransparent(true);
	}
	
	ConversationSound::init();
	registerGraphics();
}

void ConversationVideo::readData(Common::SeekableReadStream &stream) {
	Common::Serializer ser(&stream, nullptr);
	ser.setVersion(g_nancy->getGameType());

	readFilename(stream, _videoName);
	readFilename(ser, _paletteName, kGameTypeVampire, kGameTypeVampire);

	ser.skip(2, kGameTypeVampire, kGameTypeNancy1);
	ser.syncAsUint16LE(_videoFormat);
	ser.skip(3); // Quality
	ser.skip(4, kGameTypeVampire, kGameTypeVampire); // paletteStart, paletteSize
	ser.syncAsUint16LE(_firstFrame);
	ser.syncAsUint16LE(_lastFrame);
	ser.skip(8, kGameTypeVampire, kGameTypeNancy1);
	ser.skip(6, kGameTypeNancy2);

	ser.skip(0x10); // Bounds
	readRect(stream, _screenPosition);

	ConversationSound::readData(stream);
}

void ConversationVideo::updateGraphics() {
	if (!_decoder.isVideoLoaded()) {
		return;
	}

	if (!_decoder.isPlaying()) {
		_decoder.start();
	}

	if (_decoder.getCurFrame() == _lastFrame) {
		_decoder.pauseVideo(true);
	}

	if (_decoder.needsUpdate()) {
		GraphicsManager::copyToManaged(*_decoder.decodeNextFrame(), _drawSurface, _videoFormat == kSmallVideoFormat);

		_needsRedraw = true;
	}

	RenderObject::updateGraphics();
}

void ConversationVideo::onPause(bool pause) {
	_decoder.pauseVideo(pause);
	RenderActionRecord::onPause(pause);
}

bool ConversationVideo::isVideoDonePlaying() {
	return _decoder.endOfVideo() || _decoder.getCurFrame() == _lastFrame;
}

Common::String ConversationVideo::getRecordTypeName() const {
	if (g_nancy->getGameType() <= kGameTypeNancy1) {
		return "PlayPrimaryVideo";
	} else {
		return "ConversationVideo";
	}
}

void ConversationCel::init() {
	registerGraphics();
	_curFrame = _firstFrame;
	_nextFrameTime = g_nancy->getTotalPlayTime();
	ConversationSound::init();
}

void ConversationCel::registerGraphics() {
	RenderObject::registerGraphics();
	_headRObj.registerGraphics();
}

void ConversationCel::updateGraphics() {
	uint32 currentTime = g_nancy->getTotalPlayTime();

	if (_state == kRun && currentTime > _nextFrameTime && _curFrame <= _lastFrame) {
		Cel &curCel = _cels[_curFrame];

		g_nancy->_resource->loadImage(curCel.bodyCelName, curCel.bodySurf, _bodyTreeName, &curCel.bodySrc, &curCel.bodyDest);
		g_nancy->_resource->loadImage(curCel.headCelName, curCel.headSurf, _headTreeName, &curCel.headSrc, &curCel.headDest);

		_drawSurface.create(curCel.bodySurf, curCel.bodySrc);
		moveTo(curCel.bodyDest);

		_headRObj._drawSurface.create(curCel.headSurf, curCel.headSrc);
		_headRObj.moveTo(curCel.headDest);

		_nextFrameTime += _frameTime;
		++_curFrame;
	}
}

void ConversationCel::readData(Common::SeekableReadStream &stream) {
	Nancy::GameType gameType = g_nancy->getGameType();
	Common::String xsheetName;
	readFilename(stream, xsheetName);
	readFilename(stream, _bodyTreeName);
	readFilename(stream, _headTreeName);
	
	uint xsheetDataSize = 0;
	byte *xsbuf = g_nancy->_resource->loadData(xsheetName, xsheetDataSize);
	if (!xsbuf) {
		return;
	}

	Common::MemoryReadStream xsheet(xsbuf, xsheetDataSize, DisposeAfterUse::YES);
	
	// Read the xsheet and load all images into the arrays
	// Completely unoptimized, the original engine uses a buffer
	xsheet.seek(0);
	Common::String signature = xsheet.readString('\0', 18);
	if (signature != "XSHEET WayneSikes") {
		warning("XSHEET signature doesn't match!");
		return;
	}

	xsheet.seek(0x22);
	uint numFrames = xsheet.readUint16LE();
	xsheet.skip(2);
	_frameTime = xsheet.readUint16LE();
	xsheet.skip(2);

	_cels.resize(numFrames);
	for (uint i = 0; i < numFrames; ++i) {
		Cel &cel = _cels[i];
		readFilename(xsheet, cel.bodyCelName);
		readFilename(xsheet, cel.headCelName);

		// Zeroes
		if (gameType >= kGameTypeNancy3) {
			xsheet.skip(74);
		} else {
			xsheet.skip(28);
		}
	}

	// Continue reading the AR stream

	// Zeroes
	if (g_nancy->getGameType() >= kGameTypeNancy3) {
		stream.skip(66);
	} else {
		stream.skip(20);
	}

	// Something related to quality
	stream.skip(3);

	_firstFrame = stream.readUint16LE();
	_lastFrame = stream.readUint16LE();

	// A few more quality-related bytes and more zeroes
	stream.skip(0x8E);

	ConversationSound::readData(stream);
}

bool ConversationCel::isVideoDonePlaying() {
	return _curFrame >= _lastFrame && _nextFrameTime <= g_nancy->getTotalPlayTime();
}
	
} // End of namespace Action
} // End of namespace Nancy
