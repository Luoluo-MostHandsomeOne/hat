// This source file is part of the 'hat' open source project.
// Copyright (c) 2016, Yuriy Vosel.
// Licensed under Boost Software License.
// See LICENSE.txt for the licence information.

#ifndef _CONFIGS_ABSTRACTION_LAYER_HPP
#define _CONFIGS_ABSTRACTION_LAYER_HPP

#include "commands_data_extraction.hpp"
#include "user_defined_layout.hpp"
#include "preprocessed_layout.hpp"

namespace hat {
namespace core {

class ConfigsAbstractionLayer
{
	LayoutUserInformation const & m_layoutInfo;
	CommandsInfoContainer const & m_commandsConfig;
public:
	ConfigsAbstractionLayer(LayoutUserInformation const & layoutInfo, CommandsInfoContainer const & commandsConfig);
	InternalLayoutRepresentation generateLayoutPresentation(size_t selectedEnv, bool isEnv_selected);
};
} //namespace core
} //namespace hat

#ifdef HAT_CORE_HEADERONLY_MODE
#include "configs_abstraction_layer.cpp"
#endif //HAT_CORE_HEADERONLY_MODE

#endif //_CONFIGS_ABSTRACTION_LAYER_HPP