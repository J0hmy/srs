## build

cd srs/trunk
rm -rf objs/srtp2 objs/openssl && ./configure --osx && make

## achive

cd srs/trunk
./configure --osx --with-hds && make

CANDIDATE=$(ifconfig en0 inet| grep inet|awk '{print $2}')
192.168.200.133
ulimit -HSn 1107
objs/srs -c conf/rtc.conf
./etc/init.d/srs status

ffmpeg -hide_banner -re -i doc/source.200kbps.768x320.flv -c copy -f flv -y rtmp://${CANDIDATE}/live/livestream
ffmpeg -hide_banner -stream_loop -1  -re -i ~/Music/Yanni-Nightingale.mkv -acodec aac -vcodec copy -f flv -y rtmp://${CANDIDATE}/live/livestream

http://${CANDIDATE}:8080/players/rtc_player.html
ffplay rtmp://${CANDIDATE}/live/livestream




## note
server side:

    ulimit -HSn 1107
    objs/srs -c conf/johnny.conf

publisher side:

    // movie
    ffmpeg -hide_banner -stream_loop 0 -re -i doc/source.200kbps.768x320.flv -c copy -f flv -y rtmp://127.0.0.1/live/livestream

    // camera
    ffmpeg -hide_banner  -f avfoundation -r 15 -s 640x480 -pix_fmt nv12 -i "0:0" -pix_fmt yuv420p -acodec aac -vcodec h264 -crf 0 -preset ultrafast  -f flv -y rtmp://127.0.0.1/live/livestream

    // clock
    ffmpeg -hide_banner  -f avfoundation -r 15 -s 640x480 -pix_fmt nv12 -i "1:0" -pix_fmt yuv420p -acodec aac -vcodec h264 -crf 0 -preset ultrafast -filter:v crop=640:480:400:300 -f flv -y rtmp://127.0.0.1/live/livestream

    // rtc
    http://127.0.0.1:8080/players/rtc_publisher.html

play side:
    
    # RTMP delta=2s
    ffplay rtmp://127.0.0.1:1935/live/livestream

    # HTTP FLV (http_remux, mount       [vhost]/[app]/[stream].flv;)
    ffplay http://127.0.0.1:8080/live/livestream.flv
    
    # http ts (http_remux, mount       [vhost]/[app]/[stream].ts;)
    ffplay http://127.0.0.1:8080/live/livestream.ts

    # HLS, delta=2s
    ffplay http://127.0.0.1:8080/live/livestream.m3u8

    # MPEG-DASH
    vlc http://127.0.0.1:8080/live/livestream.mpd
    
    # hds (https://github.com/ossrs/srs/issues/1535)
    unknown

