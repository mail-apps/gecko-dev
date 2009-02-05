function handleRequest(request, response)
{
  response.setStatusLine(request.httpVersion, 307, "Moved temporarly");
  response.setHeader("Location", "http://example.com/non-existing-dynamic.html");
  response.setHeader("Content-Type", "text/html");
}
