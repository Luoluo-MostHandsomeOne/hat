// This source file is part of the 'hat' open source project.
// Copyright (c) 2016, Yuriy Vosel.
// Licensed under Boost Software License.
// See LICENSE.txt for the licence information.

#include "engine.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include "../external_dependencies/robot/Source/Keyboard.h"
#include "../external_dependencies/robot/Source/Mouse.h"
#include "../external_dependencies/robot/Source/Timer.h"

#ifdef HAT_WINDOWS_SCANCODES_SUPPORT
#include <windows.h>
#endif
namespace hat {
namespace tool {
#ifdef HAT_WINDOWS_SCANCODES_SUPPORT
extern bool SHOULD_USE_SCANCODES;
#endif
	Engine::Engine(hat::core::LayoutUserInformation const & layoutInfo,
		hat::core::CommandsInfoContainer const & commandsConfig, bool stickEnvToWindow, unsigned int keystrokes_delay) :
		m_selectedEnvironment(0), isEnv_selected(false),
		m_stickEnvToWindow(stickEnvToWindow),
		m_keystrokes_delay(keystrokes_delay),
		m_layoutInfo(layoutInfo),
		m_commandsConfig(commandsConfig)
	{
		if (!shouldShowEnvironmentSelectionPage()) { // if there is only one environment, we don't need to select anything
			setNewEnvironment(0);
		}
		auto environmentsCount = m_commandsConfig.getEnvironments().size();
		m_stickInfo.reserve(environmentsCount);
		for (size_t i = 0; i < environmentsCount; ++i) {
			m_stickInfo.push_back(0);
		}
	}

	bool Engine::shouldShowEnvironmentSelectionPage() const
	{
		return m_commandsConfig.m_environments.size() > 1;
	}

	bool Engine::setNewEnvironment(size_t environmentIndex)
	{
		if ((environmentIndex != m_selectedEnvironment) || (!isEnv_selected)) {
			m_selectedEnvironment = environmentIndex;
			isEnv_selected = true;
			m_shouldRebuildNormalLayout = true;
			if (m_stickEnvToWindow) {
				m_currentState = LayoutState::STICK_ENVIRONMENT_TO_WND;
			} else {
				m_currentState = LayoutState::NORMAL;
			}
			return true;
		}
		return false;
	}

	bool Engine::canSendTheCommmandForEnvironment() const
	{
		if (m_stickEnvToWindow) {
			return ROBOT_NS::Window::GetActive().GetHandle() == m_stickInfo[m_selectedEnvironment];
		}
		return true;
	}

	void Engine::switchLayout_wrongTopmostWindow()
	{
		m_shouldRebuildNormalLayout = false;
		m_currentState = LayoutState::WAIT_FOR_EXPECTED_WND_AT_FRONT;
	}

	void Engine::switchLayout_restoreToNormalLayout()
	{
		m_currentState = LayoutState::NORMAL;
		m_currentNormalLayout.setStartLayoutPage(*m_lastTopPageSelected);
	}

	void Engine::stickCurrentTopWindowToSelectedEnvironment()
	{
		m_stickInfo[m_selectedEnvironment] = ROBOT_NS::Window::GetActive().GetHandle();
		m_currentState = LayoutState::NORMAL;
	}

	void Engine::executeCommandForCurrentlySelectedEnvironment(size_t commandIndex)
	{
		hat::core::Command commandToExecute = m_commandsConfig.m_commandsList[commandIndex];

		auto & hotkeyToExecute = commandToExecute.hotkeysForEnvironments[m_selectedEnvironment];
		
		if (hotkeyToExecute->enabled) {
			std::cout << "Sequence for the command (id='" << commandToExecute.commandID.getValue()
				<< "') decoded by Robot library. Simulating the sequence:\n\t" << hotkeyToExecute->m_value << "\n";
			hotkeyToExecute->execute();


			// Do the variable operations and updating their values in UI.
			auto & variablesManager = m_commandsConfig.getVariablesManagers().getManagerForEnv(m_selectedEnvironment);
			auto changedVariables = variablesManager.executeCommandAndGetChangedVariablesList(commandIndex);
			for (auto & updatedVariableID : changedVariables) {
				auto & listOfElementsToRefresh = m_currentlyDisplayedVariables[updatedVariableID];
				auto newValue = variablesManager.getValue(updatedVariableID);
				for (auto & elementIDToRefresh : listOfElementsToRefresh) {
					if (m_uiNotesUpdater) {
						m_uiNotesUpdater(elementIDToRefresh, newValue);
					}
				}
			}
		}
	}

	bool Engine::canStickToWindows()
	{
		return ROBOT_NS::Window::IsAxEnabled();
	}
	namespace {
		// This method parses the string representing a mouse input.
		// The format of the string should look like this: "L@434,234" for the mouse clicks.
		// The substring before '@' represents the type of the button, numbers after it - the coordinates of the click.
		auto parseMouseInputCommandInfoString(std::string const & toProcess)
		{
			std::stringstream myStream(toProcess);
			ROBOT_NS::Button buttonType;
			{
				auto buttonTypeStr = std::string{};
				std::getline(myStream, buttonTypeStr, '@');
				if (buttonTypeStr == core::ConfigFilesKeywords::MouseButtonTypes::LeftButton()) {
					buttonType = ROBOT_NS::Button::ButtonLeft;
				} else if (buttonTypeStr == core::ConfigFilesKeywords::MouseButtonTypes::RightButton()) {
					buttonType = ROBOT_NS::Button::ButtonRight;
				} else if (buttonTypeStr == core::ConfigFilesKeywords::MouseButtonTypes::MiddleButton()) {
					buttonType = ROBOT_NS::Button::ButtonMid;
				} else if (buttonTypeStr == core::ConfigFilesKeywords::MouseButtonTypes::X1Button()) {
					buttonType = ROBOT_NS::Button::ButtonX1;
				} else if (buttonTypeStr == core::ConfigFilesKeywords::MouseButtonTypes::X2Button()) {
					buttonType = ROBOT_NS::Button::ButtonX2;
				} else {
					std::stringstream error;
					error << "Unknown mouse button type specified: " << buttonTypeStr;
					throw std::runtime_error(error.str());
				}
			}
			
			ROBOT_NS::Point coord;
			{
				auto x_str = std::string{};
				auto y_str = std::string{};
				std::getline(myStream, x_str, ',');
				std::getline(myStream, y_str);
				coord.X = std::stoi(x_str);
				coord.Y = std::stoi(y_str);
			}

			return std::make_pair(buttonType, coord);
		}
	}
	Engine Engine::create(std::string const & commandsCSV, std::vector<std::string> const & inputSequencesConfigs, std::vector<std::string> const & variablesManagersSetupConfigs, std::string const & layoutConfig, bool stickEnvToWindow, unsigned int keyboard_intervals)
	{
		std::fstream csvStream(commandsCSV.c_str());
		if (!csvStream.is_open()) {
			throw std::runtime_error("Could not find or open the commands config file: " + commandsCSV);
		}

		class MyHotkeyCombination: public core::SimpleHotkeyCombination
		{
			ROBOT_NS::KeyList m_sequence;
			unsigned int m_keystrokes_delay;
			//little helper function, which abstracts away the keyboard simulation part (which can be platform-dependant)
			void simulateSingleKeyboardInputEvent(ROBOT_NS::Keyboard & keyboard, std::pair<bool, ROBOT_NS::Key> const & keyboardEvent) {
#ifdef HAT_WINDOWS_SCANCODES_SUPPORT
				//optional behaviour:
				if (SHOULD_USE_SCANCODES) {
					bool isExtendedKey =
						(keyboardEvent.second == VK_UP) ||
						(keyboardEvent.second == VK_DOWN) ||
						(keyboardEvent.second == VK_LEFT) ||
						(keyboardEvent.second == VK_RIGHT) ||
						(keyboardEvent.second == VK_HOME) ||
						(keyboardEvent.second == VK_END) ||
						(keyboardEvent.second == VK_PRIOR) ||
						(keyboardEvent.second == VK_NEXT) ||
						(keyboardEvent.second == VK_INSERT) ||
						(keyboardEvent.second == VK_DELETE);

					INPUT input = { 0 };
					input.type = INPUT_KEYBOARD;
					// Calculate scan-code from the Robot's virtual key code:
					input.ki.wScan = MapVirtualKey(keyboardEvent.second, MAPVK_VK_TO_VSC);
					input.ki.dwFlags = (keyboardEvent.first) ? 
						KEYEVENTF_SCANCODE : (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP);
					if (isExtendedKey) {
						input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
					}
					
					SendInput (1, &input, sizeof (INPUT));
					ROBOT_NS::Timer::Sleep (m_keystrokes_delay);
					return;
				}
#endif
				//default behaviour:
				(keyboardEvent.first) ? keyboard.Press(keyboardEvent.second) : keyboard.Release(keyboardEvent.second);
			}
		public:
			MyHotkeyCombination(std::string const & param, bool isEnabled, ROBOT_NS::KeyList const & sequence, unsigned int keystrokes_delay): core::SimpleHotkeyCombination(param, isEnabled), m_sequence(sequence), m_keystrokes_delay(keystrokes_delay) {
			}
			void execute() override {
				if (enabled) {
					auto keyboard = ROBOT_NS::Keyboard{};
					keyboard.AutoDelay = m_keystrokes_delay;
					for (auto const & key_event : m_sequence) {
						simulateSingleKeyboardInputEvent(keyboard, key_event);
					}
				}
			}
		};

		class MyMouseInput: public core::SimpleMouseInput
		{
			ROBOT_NS::Button m_buttonToClick;
			ROBOT_NS::Point m_scr_coord;
			unsigned int m_delay;
		public:
			MyMouseInput(std::string const & param, bool isEnabled,
				ROBOT_NS::Button buttonToClick, ROBOT_NS::Point const & pos,
				unsigned int delayAfterClick) :
					core::SimpleMouseInput(param, isEnabled), m_buttonToClick(buttonToClick),
					m_scr_coord(pos), m_delay(delayAfterClick) {
			}
			void execute() override {
				if (enabled) {
					ROBOT_NS::Mouse mouse;
					mouse.SetPos(m_scr_coord);
					mouse.Click(m_buttonToClick);

					// Adding the wait operation after each click (for consistency sake):
					ROBOT_NS::Timer::Sleep (m_delay);
				}
			}
		};

		auto lambdaForKeyboardInputObjectsCreation = [&] (std::string const & param, core::CommandID const & commandID, size_t ) {
			ROBOT_NS::KeyList sequence;
			auto result = ROBOT_NS::Keyboard::Compile(param.c_str(), sequence);
			if (!result) {
				std::cout << "Error during decoding of the sequence for the command (id='"
					<< commandID.getValue() << "') by Robot library:\n\t" << param << "\nThe sequence will be disabled.\n";
			}
			auto shouldEnable = result && (param.size() > 0);
			return std::make_shared<MyHotkeyCombination>(param, shouldEnable, sequence, keyboard_intervals);
		};

		auto lambdaForMouseInputObjectsCreation = [&] (std::string const & param, core::CommandID const & commandID, size_t ) {
			auto parameterizationParseResult = parseMouseInputCommandInfoString(param);
			//TODO:add support for scrolling also
			return std::make_shared<MyMouseInput>(param, true, parameterizationParseResult.first, parameterizationParseResult.second, keyboard_intervals);
		};

		auto commandsConfig = hat::core::CommandsInfoContainer::parseConfigFile(csvStream, lambdaForKeyboardInputObjectsCreation);

		for (auto & inputSequencesConfig: inputSequencesConfigs) {
			if (inputSequencesConfig.size() > 0) {
				std::cout << "Starting to read input sequences file '" << inputSequencesConfig << "'\n";
				std::fstream typingsSequensesConfigStream(inputSequencesConfig.c_str());
				if (typingsSequensesConfigStream.is_open()) {
					commandsConfig.consumeInputSequencesConfigFile(typingsSequensesConfigStream, lambdaForKeyboardInputObjectsCreation, lambdaForMouseInputObjectsCreation);
				} else {
					std::cout << "  ERROR: file could not be opened. Please check the path.\n";
				}
			}
		}

		for (auto & variablesManagersConfig: variablesManagersSetupConfigs) { // TODO: [low priority] refactoring: get rid of the code duplication (see loop above)
			if (variablesManagersConfig.size() > 0) {
				std::cout << "Starting to read varaibles managers config file '" << variablesManagersConfig << "'\n";
				std::fstream filestream(variablesManagersConfig.c_str()); 
				if (filestream.is_open()) {
					commandsConfig.consumeVariablesManagersConfig(filestream);
				} else {
					std::cout << "  ERROR: could not be opened. Please check the path.\n";
				}
			}
		}

		std::fstream configStream(layoutConfig.c_str());
		if (!configStream.is_open()) {
			throw std::runtime_error("Could not find or open the layout config file: " + layoutConfig);
		}
		auto layout = hat::core::LayoutUserInformation::parseConfigFile(configStream);
		return Engine(layout, commandsConfig, stickEnvToWindow, keyboard_intervals);
	}

	namespace {
		struct IDsForNavigation
		{
			tau::common::LayoutPageID m_fallbackId;
			tau::common::LayoutPageID m_currentPageID;

			IDsForNavigation(tau::common::LayoutPageID const & fallbackID, tau::common::LayoutPageID const & thisPageID) :
				m_fallbackId(fallbackID), m_currentPageID(thisPageID) {}
			IDsForNavigation createNewSelector(tau::common::LayoutPageID const & newID) const
			{
				//if there is no fallback id, then we are inside generation of the top layout page, so all the selector pages, genetared from it, should get it's ID as an initial fallback.
				return hasFallbackID() ? IDsForNavigation(m_fallbackId, newID) : IDsForNavigation(m_currentPageID, newID);
			}
			bool hasFallbackID() const {
				return m_fallbackId.getValue().size() > 0;
			}
		};
	}
	std::string Engine::getCurrentLayoutJson() const {
		if (m_currentState == LayoutState::NORMAL) {
			if (m_shouldRebuildNormalLayout) {
				m_shouldRebuildNormalLayout = false;
				return getCurrentLayoutJson_normal();
			} else {
				return m_currentNormalLayout.getJson();
			}
		} else if (m_currentState == LayoutState::STICK_ENVIRONMENT_TO_WND) {
			return getCurrentLayoutJson_waitForTopWindowInfo();
		} else if (m_currentState == LayoutState::WAIT_FOR_EXPECTED_WND_AT_FRONT) {
			return getCurrentLayoutJson_wrongTopWindowMessage();
		}
		std::cerr << "Program in invalid state. We should never get here. Exiting.\n";
		abort();
	}

	std::string Engine::getCurrentLayoutJson_wrongTopWindowMessage() const
	{
		namespace lg = tau::layout_generation;
		auto topElem = lg::EvenlySplitLayoutElementsContainer{ true };
		std::stringstream headerMessage;
		headerMessage << "Wrong topmost window for current environment: please bring the window for " << m_commandsConfig.getEnvironments()[m_selectedEnvironment];
		topElem.push(lg::LabelElement(headerMessage.str()));
		topElem.push(lg::ButtonLayoutElement().note("Cancel the current pending command").ID(tau::common::ElementID(generateClearPendingCommandButtonID())));
		topElem.push(lg::ButtonLayoutElement().note("Expected window now on top. Retry the command").ID(tau::common::ElementID(generateResendPendingCommandButtonID())));
		auto resultLayout = lg::LayoutInfo{};
		resultLayout.pushLayoutPage(lg::LayoutPage(tau::common::LayoutPageID(generateTrowawayTauIdentifier()), topElem));
		return resultLayout.getJson();
	}

	std::string Engine::getCurrentLayoutJson_waitForTopWindowInfo() const
	{
		//TODO: get rid of some of the code duplication with the previous method.
		namespace lg = tau::layout_generation;
		auto topElem = lg::EvenlySplitLayoutElementsContainer{ true };
		std::stringstream headerMessage;
		headerMessage << "Please move the target window for " << m_commandsConfig.getEnvironments()[m_selectedEnvironment] << " to the top.";
		topElem.push(lg::LabelElement(headerMessage.str()));
		topElem.push(lg::ButtonLayoutElement().note("Expected window now on top. Proceed").ID(tau::common::ElementID(generateStickEnvironmentToWindowCommand())));
		auto resultLayout = lg::LayoutInfo{};
		resultLayout.pushLayoutPage(lg::LayoutPage(tau::common::LayoutPageID(generateTrowawayTauIdentifier()), topElem));
		return resultLayout.getJson();
	}

	std::string Engine::getCurrentLayoutJson_normal() const
	{
		using namespace std::string_literals;
		
		m_currentlyDisplayedVariables.clear(); // Each time we generate the new normal layout, we have to refresh this mapping.

		hat::core::ConfigsAbstractionLayer layer(m_layoutInfo, m_commandsConfig);
		auto currentLayoutState = layer.generateLayoutPresentation(m_selectedEnvironment, isEnv_selected);

		size_t const TOP_PAGES_COUNT = currentLayoutState.getPages().size();
		TOP_PAGES_IDS.clear(); // these are the pages, which are valid for restoring state to (other layout pages like the options selectors should not be restored to)
		TOP_PAGES_IDS.reserve(TOP_PAGES_COUNT);
		for (size_t i = 0; i < TOP_PAGES_COUNT; ++i) {
			TOP_PAGES_IDS.push_back(tau::common::LayoutPageID(generateTrowawayTauIdentifier()));
		}

		m_currentNormalLayout = tau::layout_generation::LayoutInfo{};

		//Can't use 'auto' for the lambda type, because this lambda is called recursively, so it's type can't be deduced
		typedef std::function<tau::layout_generation::EvenlySplitLayoutElementsContainer(hat::core::InternalLayoutPageRepresentation const &, IDsForNavigation const & navigationIDs)> MyLambdaType;
		MyLambdaType createLayoutPage =
			[&](hat::core::InternalLayoutPageRepresentation const & userData, IDsForNavigation const & navigationIDs) -> tau::layout_generation::EvenlySplitLayoutElementsContainer {
			auto result = tau::layout_generation::EvenlySplitLayoutElementsContainer{ true };
			for (auto const & row : userData.getLayout()) {
				auto newElementsRow = tau::layout_generation::EvenlySplitLayoutElementsContainer{ false };
				for (auto const & elem : row) {
					
					// Simple lambda, which records the label id with the variable id, which this label represents
					auto linkIdWithTextVariableIfNeeded = [&](tau::common::ElementID const & elementID) {
						if (elem.referencesVariable()) {
							auto variableID = elem.getReferencedVariable();
							m_currentlyDisplayedVariables[variableID].push_back(elementID);
						}
					};

					if (elem.is_button()) {
						auto toPush = tau::layout_generation::ButtonLayoutElement();
						toPush.note(hat::core::escapeRawUTF8_forJson(elem.getNote()));
						if (elem.isActive()) {
							if (elem.getReferencedCommand().nonEmpty()) {
								size_t commandIndex = m_commandsConfig.getCommandIndex(elem.getReferencedCommand());
								auto idToUse = tau::common::ElementID{generateTauIdentifierForCommand(commandIndex)};
								linkIdWithTextVariableIfNeeded(idToUse);
								toPush.ID(idToUse);
								if (navigationIDs.hasFallbackID()) {
									//Since this button does not swtich to any page explicitly, we add an automatic fallback switch here
									toPush.switchToAnotherLayoutPageOnClick(navigationIDs.m_fallbackId);
								}
							}
							auto optionsSelectorForElem = elem.getOptionsPagePtr();
							if (optionsSelectorForElem != nullptr) {
								auto newPageID = tau::common::LayoutPageID{ generateTrowawayTauIdentifier() };
								auto newNav = navigationIDs.createNewSelector(newPageID);
								auto newLayoutPage = createLayoutPage(*optionsSelectorForElem, newNav);

								//decorate the page here:
								auto layoutDecorations = tau::layout_generation::EvenlySplitLayoutElementsContainer(true);
								layoutDecorations.push(tau::layout_generation::LabelElement(hat::core::escapeRawUTF8_forJson(elem.getNote())));
								layoutDecorations.push(tau::layout_generation::ButtonLayoutElement()
									.note("back").switchToAnotherLayoutPageOnClick(navigationIDs.m_currentPageID));

								m_currentNormalLayout.pushLayoutPage(tau::layout_generation::LayoutPage(newNav.m_currentPageID,
									tau::layout_generation::UnevenlySplitElementsPair(newLayoutPage, layoutDecorations, true, 0.75)
								));
								toPush.switchToAnotherLayoutPageOnClick(newNav.m_currentPageID);
							}

							auto anotherEnvironmentSwitchingInfo = elem.switchingToAnotherEnvironment_info();
							if (anotherEnvironmentSwitchingInfo.first) {
								auto idToUse = tau::common::ElementID(generateTauIdentifierForEnvSwitching(anotherEnvironmentSwitchingInfo.second));
								linkIdWithTextVariableIfNeeded(idToUse);
								toPush.ID(idToUse);
							}
						} else {
							toPush.setEnabled(false);
						}
						newElementsRow.push(toPush);
					} else {
						//TODO: clean up the code for registering the IDs. Currently we have to call manually linkIdWithTextVariableIfNeeded() function. It's logic should be applied automatically when we generate the ElementID here (and in the code above).
						auto idToUse = tau::common::ElementID(generateTrowawayTauIdentifier());
						linkIdWithTextVariableIfNeeded(idToUse);
						auto elementToAdd = tau::layout_generation::LabelElement(hat::core::escapeRawUTF8_forJson(elem.getNote()));
						elementToAdd.ID(idToUse); // don't forget to assign the ID to the label
						newElementsRow.push(elementToAdd);
					}
				}
				result.push(newElementsRow);
			}
			return result;
		}; // end of lambda that holds the logic for generating the user-defined contents for the layout page (without navigation buttons and auto-generated info label)

		auto selectedEnvCaptionPrefix = isEnv_selected ? ("["s + m_commandsConfig.getEnvironments()[m_selectedEnvironment] + "] "s) : ""s;
		
		auto emptyFallbackID = tau::common::LayoutPageID{ "" };
		for (size_t i = 0; i < TOP_PAGES_COUNT; ++i) {
			auto currentPageID = TOP_PAGES_IDS[i];
			auto & currentPreprocessedPagePresentation = currentLayoutState.getPages()[i];

			auto navigationInfo = IDsForNavigation{ emptyFallbackID, currentPageID };
			auto contents = createLayoutPage(currentPreprocessedPagePresentation, navigationInfo); ///TODO: add m_selectedEnvironment use here

			if (TOP_PAGES_COUNT > 1) {
				auto navigationButtons = tau::layout_generation::EvenlySplitLayoutElementsContainer(false);
				auto pushNavigationButton = [&](size_t destIndex)
				{
					if (destIndex < TOP_PAGES_COUNT) {
						navigationButtons.push(
							tau::layout_generation::ButtonLayoutElement().note(
								hat::core::escapeRawUTF8_forJson(currentLayoutState.getPages()[destIndex].getNote()))
							.switchToAnotherLayoutPageOnClick(tau::common::LayoutPageID(TOP_PAGES_IDS[destIndex])));
					} else {
						navigationButtons.push(tau::layout_generation::EmptySpace());
					}
				};
				if (i == 0) {
					navigationButtons.push(tau::layout_generation::UnevenlySplitElementsPair(
						tau::layout_generation::ButtonLayoutElement().note("reload").ID(tau::common::ElementID(generateReloadButtonID())),
						tau::layout_generation::EmptySpace(), false, 0.75));
				} else {
					pushNavigationButton(i - 1); // this overflows at zero position, but since the unsigned int overflow is well-defined, we don't have a problem here
				}
				pushNavigationButton(i + 1);

				auto layoutDecorations = tau::layout_generation::UnevenlySplitElementsPair(
					tau::layout_generation::LabelElement(
						hat::core::escapeRawUTF8_forJson(selectedEnvCaptionPrefix + currentPreprocessedPagePresentation.getNote())),
					navigationButtons,
					true, 0.4);

				m_currentNormalLayout.pushLayoutPage(tau::layout_generation::LayoutPage(currentPageID,
					tau::layout_generation::UnevenlySplitElementsPair(contents, layoutDecorations, true, 0.75)
				));
			} else {
				m_currentNormalLayout.pushLayoutPage(tau::layout_generation::LayoutPage(currentPageID, contents));
			}
		}

		if ((TOP_PAGES_COUNT > 1) && (m_commandsConfig.getEnvironments().size() > 1)) {
			m_lastTopPageSelected = TOP_PAGES_IDS.begin() + 1;
		} else {
			m_lastTopPageSelected = TOP_PAGES_IDS.begin();
		}
		m_currentNormalLayout.setStartLayoutPage(*m_lastTopPageSelected);

		auto resultString = m_currentNormalLayout.getJson();
		return resultString;
	}

	void Engine::layoutPageSwitched(tau::common::LayoutPageID const & pageID)
	{
		auto findResult = std::find(TOP_PAGES_IDS.begin(), TOP_PAGES_IDS.end(), pageID);
		if (findResult != TOP_PAGES_IDS.end()) {
			m_lastTopPageSelected = findResult;
		}
	}

	void Engine::addNoteUpdatingFeedbackCallback(std::function<void (tau::common::ElementID const &, std::string)> callback)
	{
		if (m_uiNotesUpdater) {
			throw std::runtime_error("Trying to re-assign the ui notes updater callback. This is not allowed and should never happen.");
		}
		m_uiNotesUpdater = callback;
	}
} //namespace tool
} //namespace hat