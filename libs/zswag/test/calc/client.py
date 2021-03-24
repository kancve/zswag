from enum import Enum
import calculator.api as api
from zswag import Client


def run(host, port):

    server_url = f"http://{host}:{port}/openapi.json"
    counter = 0
    failed = 0
    print(f"[py-test-client] Connecting to {server_url}", flush=True)

    def run_test(aspect, request, fn, expect):
        nonlocal counter, failed
        counter += 1
        try:
            print(f"[py-test-client] Test#{counter}: {aspect}", flush=True)
            print(f"[py-test-client]   -> Instantiating client.", flush=True)
            client = api.Calculator.Client(Client(f"http://{host}:{port}/openapi.json"))
            print(f"[py-test-client]   -> Running request.", flush=True)
            resp = fn(client, request, request)
            if resp.value == expect:
                print(f"[py-test-client]   -> Success.", flush=True)
            else:
                raise ValueError(f"Expected {expect}, got {resp.value}!")
        except Exception as e:
            failed += 1
            print(f"[py-test-client]   -> ERROR: {e}", flush=True)

    run_test(
        "Pass fields in path and query",
        api.BaseAndExponent(api.I32(2), api.I32(3)),
        api.Calculator.Client.powerMethod,
        8.)

    run_test(
        "Pass hex-encoded array in query",
        api.Integers([100, -200, 400]),
        api.Calculator.Client.isumMethod,
        300.)

    run_test(
        "Pass base64url-encoded byte array in path",
        api.Bytes([8, 16, 32, 64]),
        api.Calculator.Client.bsumMethod,
        120.)

    run_test(
        "Pass base64-encoded long array in path",
        api.Integers([1, 2, 3, 4]),
        api.Calculator.Client.imulMethod,
        24.)

    run_test(
        "Pass float array in query.",
        api.Doubles([34.5, 2.]),
        api.Calculator.Client.fmulMethod,
        69.)

    run_test(
        "Pass bool array in query (expect false).",
        api.Bools([True, False]),
        api.Calculator.Client.bmulMethod,
        False)

    run_test(
        "Pass bool array in query (expect true).",
        api.Bools([True, True]),
        api.Calculator.Client.bmulMethod,
        True)

    run_test(
        "Pass request as blob in body",
        api.Double(1.),
        api.Calculator.Client.identityMethod,
        1.)

    run_test(
        "Pass base64-encoded strings.",
        api.Strings(["foo", "bar"]),
        api.Calculator.Client.concatMethod,
        "foobar")

    if failed > 0:
        print(f"[py-test-client] Done, {failed} test(s) failed!", flush=True)
        exit(1)
    else:
        print(f"[py-test-client] All tests succeeded.!", flush=True)
        exit(0)

