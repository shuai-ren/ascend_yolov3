import sys
import os
import time
import argparse


parser = argparse.ArgumentParser()
parser.add_argument('--rtsp_port',
                default=None, type=int,
                help='The port of rtsp') 

def alter(file, old_str, new_str):
    """
    替换文件中的字符串
    :param file:文件名
    :param old_str:就字符串
    :param new_str:新字符串
    :return:
    """
    file_data = ""
    with open(file, "r", encoding="utf-8") as f:
        for line in f:
            if old_str in line:
                line = line.replace(old_str,new_str)
            file_data += line
    with open(file,"w",encoding="utf-8") as f:
        f.write(file_data)

if __name__ == "__main__":
    args = parser.parse_args()
    rtsp_port = args.rtsp_port

    f = open('./mediamtx.yml','r')
    lines = f.readlines()
    for lines in lines:
        if "rtspAddress: :" in lines:
            l_port = lines
            print("linesssss:", lines)
    alter("mediamtx.yml", str(l_port), "rtspAddress: :" + str(rtsp_port)+'\n')
    pwd = os.getcwd()
    os.system('nohup '+ str(pwd) + '/mediamtx > mediamtx.log 2>&1 &')