# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import hashlib
import http.server
from pathlib import Path
import runpy
import tempfile
import threading
import unittest
from unittest.mock import patch

MODULE=runpy.run_path(str(Path(__file__).resolve().parents[2]/'tools/apkg-portal-links'))
address,download,Refusal=MODULE['address'],MODULE['download'],MODULE['Refusal']

class LinkedArchives(unittest.TestCase):
 def test_dns_is_checked_for_all_answers_and_hosts_and_credentials_are_not_bypassed(self):
  answers=lambda ips:[(2,1,6,'',(ip,443)) for ip in ips]
  for ips in (['127.0.0.1'],['169.254.169.254'],['10.0.0.1'],['93.184.216.34','127.0.0.1']):
   with self.subTest(ips=ips),patch('socket.getaddrinfo',return_value=answers(ips)):
    with self.assertRaises(Refusal):address('https://example.org/archive.pkg')
  with patch('socket.getaddrinfo',return_value=answers(['93.184.216.34'])):
   self.assertEqual(address('https://releases.example.org/a.pkg',['example.org'])[1],['93.184.216.34'])
   for url in ('https://example.org.evil/a.pkg','https://user:secret@example.org/a.pkg','file:///archive','http://example.org/a.pkg','https://example.org/a.pkg#fragment'):
    with self.assertRaises(Refusal):address(url,['example.org'])

 def test_loopback_is_explicit_and_downloads_check_exact_size_digest_redirects_and_cleanup(self):
  payload=b'test archive bytes'
  class Handler(http.server.BaseHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_GET(self):
    if self.path=='/redirect':
     self.send_response(302);self.send_header('Location','/archive');self.end_headers();return
    if self.path=='/bad-scheme':
     self.send_response(302);self.send_header('Location','file:///private');self.end_headers();return
    self.send_response(200);self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
  thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:
   url='http://127.0.0.1:'+str(server.server_port)
   expected=(hashlib.sha256(payload).hexdigest(),len(payload))
   with tempfile.TemporaryDirectory() as temporary:
    path=Path(temporary)/'archive'
    with self.assertRaises(Refusal):download(url+'/archive',path,expected)
    self.assertFalse(path.exists())
    download(url+'/redirect',path,expected,allow_loopback=True);self.assertEqual(path.read_bytes(),payload);path.unlink()
    for source,identity in ((url+'/bad-scheme',expected),(url+'/archive',('0'*64,len(payload))),(url+'/archive',(expected[0],len(payload)-1))):
     with self.assertRaises(Refusal):download(source,path,identity,allow_loopback=True)
     self.assertFalse(path.exists())
  finally:server.shutdown();server.server_close();thread.join()

 def test_checked_numeric_address_is_used_for_the_socket_and_tls_verifies_the_original_host(self):
  class Response:
   status=200
   def getheader(self,name,default=None):return '0' if name=='Content-Length' else default
   def read(self,size):return b''
  class Connection:
   def __init__(self,host,port,timeout,context):
    self.host=host
    assert context.check_hostname and context.verify_mode==2
   def request(self,*args,**kwargs):self._create_connection((self.host,443),20)
   def getresponse(self):return Response()
   def close(self):pass
  with tempfile.TemporaryDirectory() as temporary,patch('socket.getaddrinfo',return_value=[(2,1,6,'',('93.184.216.34',443))]) as dns,patch('socket.create_connection') as connect,patch('http.client.HTTPSConnection',Connection):
   download('https://example.org/a.pkg',Path(temporary)/'archive',(hashlib.sha256(b'').hexdigest(),0))
   self.assertEqual(dns.call_count,1)
   self.assertEqual(connect.call_args.args[0],('93.184.216.34',443))
