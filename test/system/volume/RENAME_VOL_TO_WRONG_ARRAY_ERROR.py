#!/usr/bin/env python3
import subprocess
import os
import sys
import json
sys.path.append("../lib/")
sys.path.append("../array/")

import json_parser
import ibofos
import cli
import test_result
import CREATE_VOL_BASIC_1
import volume

NAME = CREATE_VOL_BASIC_1.VOL_NAME
SIZE = CREATE_VOL_BASIC_1.VOL_SIZE
IOPS = CREATE_VOL_BASIC_1.VOL_IOPS
BW = CREATE_VOL_BASIC_1.VOL_BW
NEW_NAME = "new_vol"

def clear_result():
    if os.path.exists( __file__ + ".result"):
        os.remove( __file__ + ".result")

def set_result(detail):
    code = json_parser.get_response_code(detail)
    result = test_result.expect_false(code)
    with open(__file__ + ".result", "w") as result_file:
        result_file.write(result + " (" + str(code) + ")" + "\n" + detail)

def execute():
    clear_result()
    CREATE_VOL_BASIC_1.execute()
    out = cli.rename_volume(NAME, NEW_NAME, "wrong_array_name")
    return out

if __name__ == "__main__":
    out = execute()
    set_result(out)
    ibofos.kill_ibofos()