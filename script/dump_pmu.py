import csv
import matplotlib.pyplot as plt
import os
import json

pmu_list = ["INST_RETIRED.ANY"]
pmu_core_after = {"INST_RETIRED.ANY": (0, 0)}
pmu_core_before = {"INST_RETIRED.ANY": (0, 0)}


def get_perfmon(path: str, pmu: list) -> dict:
    data_dict = {}
    cur_csv = json.loads(f.read())

    with open(path, "r") as f:
        for line in pmu:
            # Extract the EventName, UMask, and EventCode
            event = cur_csv["Events"][0]
            event_name = event["EventName"]
            umask = event["UMask"]
            event_code = event["EventCode"]

            # Combine UMask and EventCode
            combined_code = (
                umask + event_code[2:]
            )  # Concatenate and remove '0x' from EventCode
            combined_code_hex = (
                "0x" + combined_code[2:]
            )  # Add '0x' back for hex representation

            # Print the results
            print(f"Event Name: {event_name}")
            print(f"Combined UMask and EventCode: {combined_code_hex}")
    return data_dict


def batch_pmu_run(pmu: dict):
    for i, p in enumerate(pmu):
        print(p)
        if i % 4 == 0:
            os.system(
                "../cmake-build-debug/CXLMemSim -t ../cmake-build-debug/microbench/ld2 -i 100 --p"+ 
            )
            os.system("mv ./output_pmu.csv ./ld_pmu2_results.csv")


if __name__ == "__main__":
    pmu = {"INST_RETIRED.ANY": 0}
    get_perfmon("./perfmon/SPR/events/sapphirerapids_core.json", pmu)
    get_perfmon("./perfmon/SPR/events/sapphirerapids_uncore.json", pmu)
    get_perfmon("./perfmon/SPR/events/sapphirerapids_uncore_experimental.json", pmu)
    # x, y = load_csv('data.csv')
    # draw_graph(x, y)
