"""Host model of the exact local device-path length/prefix condition.

No firmware execution and no device access. The model intentionally excludes
unknown runtime handles; it proves why the published flat GOP handle itself
cannot satisfy the parent path requested by BmConsole.
"""
import json
import struct
import uuid
from pathlib import Path

VENDOR_GUID = uuid.UUID("3fa80f1d-6627-41de-a6ef-71caa95721d4")
vendor = struct.pack("<BBH", 1, 4, 20) + VENDOR_GUID.bytes_le
# ACPI_DISPLAY_ADR(1, 0, 0, 1, 0, INTERNAL_DIGITAL=4, 0, 0).
# Its payload value cannot affect the demonstrated size mismatch.
adr = struct.pack("<BBHI", 2, 3, 8, (1 << 31) | (1 << 16) | (4 << 8))
end = bytes((0x7F, 0xFF, 4, 0))
flat_gop = vendor + adr + end
requested_parent = vendor + end


def payload_before_end(path):
    off = 0
    while True:
        kind, subtype, size = struct.unpack_from("<BBH", path, off)
        if kind == 0x7F:
            assert subtype == 0xFF and size == 4
            return path[:off]
        assert size >= 4 and off + size <= len(path)
        off += size


def candidate_matches_request(handle_path, requested_path):
    # CoreLocateDevicePath: Locate.c514-516.
    handle_payload = payload_before_end(handle_path)
    source_payload = payload_before_end(requested_path)
    return len(handle_payload) <= len(source_payload) and (
        source_payload[: len(handle_payload)] == handle_payload
    )


assert len(flat_gop) == 32
assert len(requested_parent) == 24
assert len(payload_before_end(flat_gop)) == 28
assert len(payload_before_end(requested_parent)) == 20
assert not candidate_matches_request(flat_gop, requested_parent)
assert candidate_matches_request(flat_gop, flat_gop)
assert candidate_matches_request(requested_parent, requested_parent)

result = {
    "scope": "STATIC_HOST_MODEL_NOT_PHYSICAL_OBSERVATION",
    "flat_gop_path_hex": flat_gop.hex(),
    "truncated_request_hex": requested_parent.hex(),
    "handle_payload_size": 28,
    "request_payload_size": 20,
    "gop_handle_matches_truncated_parent": False,
    "gop_handle_matches_full_request": True,
    "hypothetical_vendor_only_parent_matches": True,
    "runtime_unknown_handles_modeled": False,
    "tests_passed": 7,
}
target = Path(__file__).with_name("FLAT_GOP_PATH_PROOF.json")
target.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
