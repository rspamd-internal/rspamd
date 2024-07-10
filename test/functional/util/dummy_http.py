#!/usr/bin/env python3

import asyncio
import dummy_killer
import tornado.ioloop
import tornado.web
import tornado.httpserver
import ssl
import argparse
import os

class MainHandler(tornado.web.RequestHandler):
    @tornado.gen.coroutine
    def get(self, path):
        if path == '/empty':
            # Return an empty reply
            self.set_header("Content-Type", "text/plain")
            self.write("")
        elif path == '/error_403':
            # Return a 403 HTTP error
            raise tornado.web.HTTPError(403)
        elif path == '/timeout':
            # Wait for 4 seconds before returning an empty reply
            yield tornado.gen.sleep(4)
            self.set_header("Content-Type", "text/plain")
            self.write("")
        elif path == '/request':
            # Return a string 'hello world'
            self.set_header("Content-Type", "text/plain")
            self.write("hello world")
        elif path == '/map-simple':
            # Return a string 'hello map'
            self.set_header("Content-Type", "text/plain")
            self.write("hello map")
        elif path == '/map-query':
            # Parse the 'key' argument from the HTTP request
            key = self.get_query_argument("key", default=None)
            if key == 'au':
                # Return a string 'hit' if 'key' is equal to 'au'
                self.set_header("Content-Type", "text/plain")
                self.write("1.0")
            else:
                # Return a 404 HTTP error if 'key' is not equal to 'au'
                raise tornado.web.HTTPError(404)
        elif path == '/settings':
            self.set_header("Content-Type", "application/json")
            self.write("{\"actions\": { \"reject\": 1.0}, \"symbols\": { \"EXTERNAL_SETTINGS\": 1.0 }}")
        else:
            raise tornado.web.HTTPError(404)

    @tornado.gen.coroutine
    def post(self, path):
        if path == '/empty':
            # Return an empty reply
            self.set_header("Content-Type", "text/plain")
            self.write("")
        elif path == '/error_403':
            # Return a 403 HTTP error
            raise tornado.web.HTTPError(403)
        elif path == '/request':
            # Return a string 'hello post'
            self.set_header("Content-Type", "text/plain")
            self.write("hello post")
        elif path == '/timeout':
            # Wait for 4 seconds before returning an empty reply
            yield tornado.gen.sleep(4)
            self.set_header("Content-Type", "text/plain")
            self.write("")
        elif path == '/map-simple':
            # Return a string 'hello map'
            self.set_header("Content-Type", "text/plain")
            self.write("hello map")
        elif path == '/map-query':
            # Parse the 'key' argument from the HTTP request
            key = self.get_query_argument("key", default="")
            if key == 'au':
                # Return a string 'hit' if 'key' is equal to 'au'
                self.set_header("Content-Type", "text/plain")
                self.write("hit")
            else:
                # Return a 404 HTTP error if 'key' is not equal to 'au'
                raise tornado.web.HTTPError(404)
        elif path == '/settings':
            self.set_header("Content-Type", "application/json")
            self.write("{\"actions\": { \"reject\": 1.0}, \"symbols\": { \"EXTERNAL_SETTINGS\": 1.0 }}")
        else:
            raise tornado.web.HTTPError(404)

    def head(self, path):
        self.set_header("Content-Type", "text/plain")
        if path == "/redirect1":
            # Send an HTTP redirect to the bind address of the server
            self.redirect(f"{self.request.protocol}://{self.request.host}/hello")
        elif path == "/redirect2":
            # Send an HTTP redirect to the bind address of the server
            self.redirect(f"{self.request.protocol}://{self.request.host}/redirect1")
        elif self.path == "/redirect3":
            # Send an HTTP redirect to the bind address of the server
            self.redirect(f"{self.request.protocol}://{self.request.host}/redirect4")
        elif self.path == "/redirect4":
            # Send an HTTP redirect to the bind address of the server
            self.redirect(f"{self.request.protocol}://{self.request.host}/redirect3")
        else:
            self.send_response(200)
        self.set_header("Content-Type", "text/plain")

def make_app():
    return tornado.web.Application([
        (r"(/[^/]+)", MainHandler),
    ])

async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", "-b", default="localhost", help="bind address")
    parser.add_argument("--port", "-p", type=int, default=18080, help="bind port")
    parser.add_argument("--keyfile", "-k", help="server private key file")
    parser.add_argument("--certfile", "-c", help="server certificate file")
    parser.add_argument("--pidfile", "-pf", help="path to the PID file")
    args = parser.parse_args()

    # Create the Tornado application
    app = make_app()

    # If keyfile and certfile are provided, create an HTTPS server.
    # Otherwise, create an HTTP server.
    if args.keyfile and args.certfile:
        ssl_ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        ssl_ctx.load_cert_chain(args.certfile, args.keyfile)
        server = tornado.httpserver.HTTPServer(app, ssl_options=ssl_ctx)
    else:
        server = tornado.httpserver.HTTPServer(app)

    # Write the PID to the specified PID file, if provided
    if args.pidfile:
        dummy_killer.write_pid(args.pidfile)

    # Start the server
    server.bind(args.port, args.bind)
    server.start(1)

    await asyncio.Event().wait()

if __name__ == "__main__":
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
