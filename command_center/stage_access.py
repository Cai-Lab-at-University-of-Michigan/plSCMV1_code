import urllib.request
import json
import time

SERVER_URL = "http://10.156.2.107:5000/"
SERVER_URL = "http://localhost:5000/"


def download_http(url):
    with urllib.request.urlopen(url) as response:
        return response.read()


def download_http_json(url):
    with urllib.request.urlopen(url) as response:
        return json.load(response)


def current_position():
    cmd = SERVER_URL + "get_positions"
    res = download_http_json(cmd)
    return {int(k): v for k, v in res.items()}


def is_moving():
    cmd = SERVER_URL + "get_is_moving"
    res = download_http_json(cmd)
    return {int(k): v for k, v in res.items()}


def any_moving():
    return any(is_moving().values())


def wait_for_move():
    while any_moving():
        time.sleep(0.05)


def move(axis: int, loc: float):
    cmd = SERVER_URL + f"move/{axis}/{loc}"
    res = download_http(cmd)
    return b"Done" in res
