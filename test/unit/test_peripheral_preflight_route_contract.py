"""Route-level contract checks for board profiles and resource preflight."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include/network/handlers/PeripheralRouteHandler.h").read_text(
    encoding="utf-8"
)
# Core mapping/preflight/param units first so body extraction anchors on the
# definitions that precede their route-layer call sites.
SOURCE = ""
for route_core_path in (
    "src/core/PeripheralRouteMapper.cpp",
    "src/core/PeripheralPreflightRouteSupport.cpp",
    "src/core/PeripheralParamApply.cpp",
):
    SOURCE += (ROOT / route_core_path).read_text(encoding="utf-8") + "\n"
SOURCE += (ROOT / "src/network/handlers/PeripheralRouteHandler.cpp").read_text(
    encoding="utf-8"
)
SOURCE += (ROOT / "src/network/handlers/PeripheralCatalogRoutes.cpp").read_text(encoding="utf-8")
SOURCE += "\n" + (ROOT / "src/network/handlers/PeripheralReadRoutes.cpp").read_text(encoding="utf-8")
CATALOG = (ROOT / "src/network/BoardProfileCatalog.cpp").read_text(encoding="utf-8")
BOARD_ROUTE = (ROOT / "src/network/handlers/BoardProfileRouteHandler.cpp").read_text(encoding="utf-8")
PLANNER_SOURCE = (ROOT / "src/core/PeripheralResourcePlanner.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature_pattern: str) -> str:
    match = re.search(signature_pattern, source)
    if not match:
        raise AssertionError(f"function not found: {signature_pattern}")
    opening = source.find("{", match.end())
    if opening < 0:
        raise AssertionError(f"function body not found: {signature_pattern}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature_pattern}")


class PeripheralPreflightRouteContractTests(unittest.TestCase):
    def test_c3_profile_is_discoverable_and_enabled_for_preflight(self):
        supported = function_body(SOURCE, 'compiledBoardProfileSupported[(][)]')
        self.assertIn('CHIP_IS_ESP32C3', supported)
        resolve = function_body(SOURCE, 'resolveBoardProfile[(]')
        self.assertIn('getEsp32C3Profile()', resolve)
        self.assertIn('profileId == "3"', resolve)
        listing = function_body(BOARD_ROUTE, 'PeripheralRouteHandler::handleGetBoardProfiles[(]')
        self.assertIn('getCompiledBoardProfile()', listing)
        self.assertIn('getEsp32C3Profile()', CATALOG)
        identity = function_body(CATALOG, 'boardProfileChip[(]')
        self.assertIn('ESP32_C3_GENERIC', identity)
        self.assertIn('"ESP32-C3"', identity)

    def test_profiles_do_not_claim_board_wiring_or_hardware_validation(self):
        serialize = function_body(CATALOG, 'writeProfile[(]')
        self.assertIn('w.flagField("hardwareVerified", false)', serialize)
        self.assertIn('w.flagField("requiresBoardWiringVerification", true)', serialize)
        self.assertIn('"chip-policy" : "board-policy"', serialize)
        resources = function_body(SOURCE, 'PeripheralRouteHandler::handleGetPeripheralResources[(]')
        self.assertIn('!sameText(profile->key, active->key)', resources)

    def test_driver_registry_capacity_is_visible_without_silent_overflow(self):
        descriptors = function_body(SOURCE, 'addSensorDriverDescriptors[(]')
        for field in ('registrationFailures', 'registeredDriverCount',
                      'driverCapacity', 'availableDriverSlots'):
            self.assertIn('FastBee::setFixedJsonField(typeObj, "' + field + '",', descriptors)

    def test_routes_are_specific_and_registered_before_generic_crud(self):
        setup = function_body(
            SOURCE, r"\bPeripheralRouteHandler::setupRoutes\s*\("
        )
        expected = (
            '"/api/boards/profiles"',
            '"/api/peripherals/resources"',
            '"/api/peripherals/preflight"',
        )
        for route in expected:
            self.assertIn(route, setup)
        generic = setup.index('"/api/peripherals"')
        for route in expected[1:]:
            self.assertLess(setup.index(route), generic)

    def test_large_fixed_preflight_result_is_request_scoped_and_guarded(self):
        self.assertNotIn("FastBee::PreflightResult preflightResult", HEADER)
        self.assertIn("SemaphoreHandle_t preflightMutex", HEADER)
        self.assertIn("~PeripheralRouteHandler", HEADER)
        constructor = function_body(
            SOURCE, r"\bPeripheralRouteHandler::PeripheralRouteHandler\s*\("
        )
        destructor = function_body(
            SOURCE, r"\bPeripheralRouteHandler::~PeripheralRouteHandler\s*\("
        )
        self.assertIn("xSemaphoreCreateMutex", constructor)
        self.assertIn("vSemaphoreDelete", destructor)
        for handler in (
            "handleGetPeripheralResources",
            "handlePreflightPeripheral",
            "handleAddPeripheral",
            "handleUpdatePeripheral",
            "handleEnablePeripheral",
            "handleImportConfiguration",
        ):
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertIn("MutexGuard resultLock(preflightMutex", body)
            self.assertIn("std::unique_ptr<FastBee::PreflightResult> workspace", body)
            self.assertIn("if (!allocatePreflightWorkspace(ctx, request, workspace)) return", body)
            self.assertLess(body.index("MutexGuard resultLock(preflightMutex"),
                            body.index("allocatePreflightWorkspace(ctx, request, workspace)"))
            self.assertLess(body.index("RecursiveMutexGuard managerLock"),
                            body.index("allocatePreflightWorkspace(ctx, request, workspace)"))

    def test_schema_exposes_stable_issues_candidate_claims_and_capacity(self):
        issues = function_body(SOURCE, r"\baddPreflightIssues\s*\(")
        for field in (
            'FastBee::setFixedJsonField(item, "code",',
            'FastBee::setFixedJsonField(item, "severity",',
            'FastBee::setFixedJsonField(item, "requiresConfirmation",',
            'FastBee::setFixedJsonField(item, "resourceKind",',
            'FastBee::setFixedJsonField(item, "ownerId",',
            'FastBee::setFixedJsonField(item, "conflictingOwnerId",',
        ):
            self.assertIn(field, issues)

        claims = function_body(SOURCE, r"\baddCandidateClaims\s*\(")
        self.assertIn("sameText(claim.ownerId, candidateId)", claims)
        for field in (
            'FastBee::setFixedJsonField(item, "resourceKind",',
            'FastBee::setFixedJsonField(item, "state",',
            'FastBee::setFixedJsonField(item, "shareMode",',
            'FastBee::setFixedJsonField(item, "role",',
            'item["pins"]',
        ):
            self.assertIn(field, claims)

        resources = function_body(SOURCE, r"\baddResourceRows\s*\(")
        for field in (
            'FastBee::setFixedJsonField(row, "used",',
            'FastBee::setFixedJsonField(row, "configured",',
            'FastBee::setFixedJsonField(row, "capacity",',
            'FastBee::setFixedJsonField(row, "available",',
            'FastBee::setFixedJsonField(row, "status",',
        ):
            self.assertIn(field, resources)

        response = function_body(SOURCE, r"\baddPreflightResponse\s*\(")
        self.assertIn("addResolvedPins(data, candidate)", response)
        resolved = function_body(SOURCE, r"\baddResolvedPins\s*\(")
        self.assertIn('data["resolvedPins"]', resolved)
        self.assertIn('data["pinRoles"]', resolved)

    def test_generic_sensor_descriptor_exposes_role_variants(self):
        descriptor = function_body(
            SOURCE, r"\baddPeripheralTypeDescriptorDetails\s*\("
        )
        self.assertIn('typeObj["pinRoleVariants"]', descriptor)
        for role in ('"DATA"', '"TRIG"', '"ECHO"', '"SDA"', '"SCL"'):
            self.assertIn(role, descriptor)

    def test_confirmations_are_specific_and_hard_errors_still_block(self):
        required = function_body(
            SOURCE, r"\brequiredAcknowledgements\s*\("
        )
        for acknowledgement in (
            "ACK_STRAPPING_PIN",
            "ACK_BOARD_SHARED_PIN",
            "ACK_UART0_CONSOLE",
            "ACK_ADC2_WITH_WIFI",
        ):
            self.assertIn(acknowledgement, required)
        self.assertNotIn('getParamBool(request, "confirmWarnings"', SOURCE)

        preflight = function_body(
            SOURCE,
            r"\bPeripheralRouteHandler::handlePreflightPeripheral\s*\(",
        )
        self.assertIn("!hasHardPreflightError(preflightResult)", preflight)
        blocked = function_body(SOURCE, r"\bsendBlockedPreflight\s*\(")
        self.assertIn('doc["code"] = "PERIPHERAL_PREFLIGHT_BLOCKED"', blocked)
        self.assertIn("sendJsonWithStatus(request, 409", blocked)

    def test_candidate_preflight_discards_existing_diagnostics_after_claims(self):
        preflight = function_body(
            PLANNER_SOURCE, r"\bpreflightPeripheralChange\s*\("
        )
        existing_claims = preflight.index("planner.addPeripheral(existing[i]")
        reset_scope = preflight.index("result.resetIssuesPreservingClaims()")
        candidate = preflight.index("planner.addPeripheral(candidate")
        self.assertLess(existing_claims, reset_scope)
        self.assertLess(reset_scope, candidate)

        resources = function_body(
            SOURCE,
            r"\bPeripheralRouteHandler::handleGetPeripheralResources\s*\(",
        )
        self.assertIn("planner.addPeripheral", resources)
        self.assertNotIn("resetIssuesPreservingClaims", resources)

    def test_add_update_and_enable_preflight_before_persistence(self):
        mutations = {
            "handleAddPeripheral": "pm.addPeripheralAndSave",
            "handleUpdatePeripheral": "pm.updatePeripheralAndSave",
            "handleEnablePeripheral": "pm.enablePeripheralAndSave",
        }
        for handler, mutation in mutations.items():
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertIn("preflightPeripheralChange", body)
            self.assertIn("sendBlockedPreflight", body)
            self.assertIn("if (compiledBoardProfileSupported())", body)
            self.assertLess(body.index("preflightPeripheralChange"), body.index(mutation))
            self.assertLess(body.index("sendBlockedPreflight"), body.index(mutation))

        for handler in ("handleAddPeripheral", "handleUpdatePeripheral"):
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertIn("applyPeripheralRequestParameters", body)

    def test_unsupported_compiled_profiles_keep_legacy_mutations_available(self):
        preflight = function_body(
            SOURCE,
            r"\bPeripheralRouteHandler::handlePreflightPeripheral\s*\(",
        )
        unsupported = preflight.index("if (!compiledBoardProfileSupported())")
        self.assertIn("ctx->sendError(request, 501", preflight[unsupported:])

        for handler in (
            "handleAddPeripheral",
            "handleUpdatePeripheral",
            "handleEnablePeripheral",
        ):
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertNotIn("if (!compiledBoardProfileSupported())", body)
            guarded = body.index("if (compiledBoardProfileSupported())")
            preflight_call = body.index("preflightPeripheralChange")
            self.assertLess(guarded, preflight_call)

    def test_selected_profile_is_read_only_and_p1_io_branches_remain(self):
        resources = function_body(
            SOURCE,
            r"\bPeripheralRouteHandler::handleGetPeripheralResources\s*\(",
        )
        self.assertIn('getParamValue(request, "profileId"', resources)
        for handler in (
            "handlePreflightPeripheral",
            "handleAddPeripheral",
            "handleUpdatePeripheral",
            "handleEnablePeripheral",
        ):
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertNotIn('getParamValue(request, "profileId"', body)
            self.assertIn("getCompiledBoardProfile", body)

        for handler in ("handleReadPeripheral", "handleWritePeripheral"):
            body = function_body(
                SOURCE, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            for peripheral_type in (
                "PeripheralType::SDIO",
                "PeripheralType::DS1302_RTC",
                "PeripheralType::RFID_MFRC522",
            ):
                self.assertIn(peripheral_type, body)
        write = function_body(
            SOURCE, r"\bPeripheralRouteHandler::handleWritePeripheral\s*\("
        )
        self.assertIn("PeripheralType::LCD1602", write)


if __name__ == "__main__":
    unittest.main()
