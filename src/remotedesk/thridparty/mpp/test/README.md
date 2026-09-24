# Unit test description

## There are some unit test for testing mpp functions in this catalog.

### mpi_enc_test:
use sync interface(poll,dequeue and enqueue), encode raw yuv to compress video.

### mpi_enc_mt_test:
multi-instance encoder test using multiple threads.

### mpi_dec_test:
use sync interface and async interface(decode_put_packet and decode_get_frame),
decode compress video to raw yuv.

### mpi_dec_mt_test:
multi-thread decoder test using decode_put_packet and decode_get_frame.

### mpi_dec_multi_test:
multi-instance decoder test, each instance runs in its own thread.

### mpi_dec_nt_test:
decoder test using sync decode interface with no timeout retry.

### mpi_rc2_test:
decode then re-encode use detailed bitrate control config, and cfg param come
from mpi_rc.cfg.

### mpi_test:
simple description of mpi calling method, just for reference

### mpp_info_test:
print MPP library version and compatibility information.

### mpp_event_trigger:
event trigger test.

### mpp_parse_cfg:
mpp parser cfg test.

### vpu_api_test
encode or decode use legacy interface, in order to compatible with the previous
vpu interface.
