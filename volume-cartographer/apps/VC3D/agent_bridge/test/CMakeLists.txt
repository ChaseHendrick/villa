# Standalone unit test for descriptor validation and the small set of parsing
# helpers used behind it. It needs no process launch or volume data.
find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(test_agent_bridge_contract
    test_agent_bridge_contract.cpp
    ../AgentBridgeMethod.cpp)

# The included app headers declare Q_OBJECT classes, but the test never
# instantiates them (it only uses the free inline helpers), so no MOC is needed.
set_target_properties(test_agent_bridge_contract PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_include_directories(test_agent_bridge_contract PRIVATE
    ${PROJECT_SOURCE_DIR}/apps/VC3D)

target_link_libraries(test_agent_bridge_contract PRIVATE
    Qt6::Core
    opencv_core
    vc_core)

add_test(NAME agent_bridge_contract COMMAND test_agent_bridge_contract)
vc_register_test_target(test_agent_bridge_contract)
vc_register_ctest(agent_bridge_contract test_agent_bridge_contract)

# command.list over real QActions in a command catalog.
find_package(Qt6 REQUIRED COMPONENTS Gui Test Widgets)

add_executable(test_agent_bridge_commands
    test_agent_bridge_commands.cpp
    ../AgentBridgeCommandCatalog.cpp
    ../../CommandCatalog.cpp
    ../../Keybinds.cpp)
set_target_properties(test_agent_bridge_commands PROPERTIES
    AUTOMOC ON AUTOUIC OFF AUTORCC OFF)
target_include_directories(test_agent_bridge_commands PRIVATE
    ${PROJECT_SOURCE_DIR}/apps/VC3D)
target_link_libraries(test_agent_bridge_commands PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test Qt6::Widgets)
add_test(NAME agent_bridge_commands COMMAND test_agent_bridge_commands)
vc_register_test_target(test_agent_bridge_commands)
vc_register_ctest(agent_bridge_commands test_agent_bridge_commands)
set_tests_properties(agent_bridge_commands PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
