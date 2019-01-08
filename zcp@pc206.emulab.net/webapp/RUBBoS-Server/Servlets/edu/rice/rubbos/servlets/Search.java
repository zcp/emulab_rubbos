/**
 * RUBBoS: Rice University Bulletin Board System.
 * Copyright (C) 2001-2004 Rice University and French National Institute For 
 * Research In Computer Science And Control (INRIA).
 * Contact: jmob@objectweb.org
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or any later
 * version.
 * 
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * Initial developer(s): Emmanuel Cecchet.
 * Contributor(s): Niraj Tolia.
 */

package edu.rice.rubbos.servlets;

import java.io.IOException;
import java.net.URLEncoder;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;

import javax.servlet.ServletException;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

//ChienAn
import org.apache.log4j.Logger;

public class Search extends RubbosHttpServlet
{
	//ChienAn
	static Logger responseTimeLogger = Logger.getLogger(Search.class);

  public int getPoolSize()
  {
    return Config.BrowseCategoriesPoolSize;
  }

  private void closeConnection(PreparedStatement stmt, Connection conn)
  {
    try
    {
      if (stmt != null)
        stmt.close(); // close statement
    }
    catch (Exception ignore)
    {
    }

    try
    {
      if (conn != null)
          releaseRConnection(conn);
    }
    catch (Exception ignore)
    {
    }

  }

  /** Build the html page for the response */
  public void doGet(HttpServletRequest request, HttpServletResponse response)
      throws IOException, ServletException
  {
	//ChienAn
	String            logMessage = request.getRequestURI() + "?urlID=" + request.getParameter("urlID");
	
    ServletPrinter    sp   = null;
    PreparedStatement stmt = null;
    Connection        conn = null;

    String testtype, search, testpage, testnbOfStories, table, title = null;
    String comment_table;
    int page = 0, type, nbOfStories = 0;
    ResultSet rs = null;

    sp = new ServletPrinter(response, "Search");

    testtype = request.getParameter("type");
    testnbOfStories = request.getParameter("nbOfStories");
    search = request.getParameter("search");
    testpage = request.getParameter("page");

    if (testtype == null)
    {
      type = 0;
    }
    else
      type = (Integer.valueOf(request.getParameter("type"))).intValue();

    if (testpage == null)
    {
      page = 0;
    }
    else
      page = (Integer.valueOf(request.getParameter("page"))).intValue();

    if (testnbOfStories != null)
    {
      nbOfStories = 25;
    }

    if (search == null)
    {
      search = request.getParameter("search");
    }

    if (testnbOfStories == null)
      nbOfStories = 25;

    sp.printHTMLheader("RUBBoS search");

    // Display the search form
    sp
        .printHTML("<form action=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search\" method=POST>\n"
            + "<center><table>\n"
            + "<tr><td><b>Search</b><td><input type=text size=50 name=search value="
            + search + ">\n" + "<tr><td><b>in</b><td><SELECT name=type>\n");
    if (type == 0)
    {
      sp.printHTML("<OPTION selected value=\"0\">Stories</OPTION>\n");
      table = "stories";
      title = "Stories";
    }
    else
      sp.printHTML("<OPTION value=\"0\">Stories</OPTION>\n");
    if (type == 1)
    {
      sp.printHTML("<OPTION selected value=\"1\">Comments</OPTION>\n");
      table = "comments";
      title = "Comments";
    }
    else
      sp.printHTML("<OPTION value=\"1\">Comments</OPTION>\n");
    if (type == 2)
    {
      sp.printHTML("<OPTION selected value=\"2\">Authors</OPTION>\n");
      table = "users";
      title = "Stories with author";
    }
    else
      sp.printHTML("<OPTION value=\"2\">Authors</OPTION>\n");
    sp.printHTML("</SELECT></table><p><br>\n"
        + "<input type=submit value=\"Search now!\"></center><p>\n");

    // Display the results
    if (search == null)
      sp
          .printHTML("<br><center><h2>Please select a text to search for</h2></center><br>");
    else
    {
      sp.printHTML("<br><h2>" + title + " matching <i>" + search
          + "</i></h2></center><br>");

      conn = getRConnection();

      if (type == 0)
      {
        try
        {
          stmt = conn.prepareStatement("SELECT stories.id, stories.title, "
              + "stories.date, users.nickname FROM"
              + " stories, users WHERE title LIKE '" + search + "%' "
              + "AND stories.writer = users.id" + /* OR body LIKE '$search%%' */
              // Yasu: for PostgreSQL
              // " ORDER BY date DESC LIMIT " + page * nbOfStories + ","
              // + nbOfStories,
//ChienAn              " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories,
              " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
              // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	      ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);
		
		//ChienAn
		long startNano = System.nanoTime();

          rs = stmt.executeQuery();

		//ChienAn
		long endNano = System.nanoTime();
	        long responseTime = (endNano - startNano)/1000;
        	logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;
        }
        catch (Exception e)
        {
          sp.printHTML("Failed to execute Query for BrowseStoriesByCategory: "
              + e);
          closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);

          return;
        }
        try
        {
          if (!rs.first())
          {
            stmt.close();
            stmt = conn
                .prepareStatement("SELECT old_stories.id, old_stories.title,"
                    + " old_stories.date, users.nickname FROM"
                    + " old_stories, users WHERE title LIKE '" + search + "%' "
                    + "AND old_stories.writer = users.id" + /*
                                                             * OR body LIKE
                                                             * '$search%%'
                                                             */
                    // Yasu: for PostgreSQL
                    // " ORDER BY date DESC LIMIT " + page * nbOfStories + ","
                    // + nbOfStories,
//ChienAn                    " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories,
                    " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
                // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	        ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);

		//ChienAn
		long startNano = System.nanoTime();
	
            rs = stmt.executeQuery();

		//ChienAn
		long endNano = System.nanoTime();
                long responseTime = (endNano - startNano)/1000;
                logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;
          }
          if (!rs.first())

          {
            if (page == 0)
              sp.printHTML("<h2>Sorry, but there is no story matching <i>"
                  + search + "</i> !</h2>");
            else
            {
              sp
                  .printHTML("<h2>Sorry, but there are no more stories available matching <i>"
                      + search + "</i>.</h2><br>\n");
              sp
                  .printHTML("<p><CENTER>\n<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?search="
                      + URLEncoder.encode(search)
                      + "&type="
                      + type
                      + "&page="
                      + (page - 1)
                      + "&nbOfStories="
                      + nbOfStories
                      + "\">Previous page</a>\n</CENTER>\n");
            }

            sp.printHTMLfooter();
            closeConnection(stmt, conn);

		//ChienAn
		if (responseTimeLogger.isInfoEnabled())
                	responseTimeLogger.info(logMessage);
            return;
          }
        }

        catch (Exception e)
        {
          sp.printHTML("Exception searching type 0: " + e + "<br>");
          closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);
          return;
        }
      } // if (type == 0)

      if (type == 1)
      { // Look for comments
        comment_table = "comments";
        try
        {
          stmt = conn
              .prepareStatement("SELECT comments.id, comments.story_id, "
                  + "comments.subject, comments.date,"
                  + " users.nickname FROM comments, "
                  + "users WHERE subject LIKE '"
                  + search + "%' " 
                  + "AND comments.writer = users.id" 
                  + /* OR comment LIKE '$search%%' */
                  // Yasu: for PostgreSQL
                  // " GROUP BY story_id ORDER BY date DESC LIMIT "
                  // + page * nbOfStories + "," + nbOfStories,
                  " ORDER BY date DESC LIMIT " + nbOfStories
//ChienAn                  + " OFFSET " + page * nbOfStories,
                  + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
              // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	      ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);
		
		//ChienAn
		long startNano = System.nanoTime();		

          rs = stmt.executeQuery();

		//ChienAn
		long endNano = System.nanoTime();
        	long responseTime = (endNano - startNano)/1000;
        	logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;

          if (!rs.first())
          {
            stmt.close();
            stmt = conn
                .prepareStatement("SELECT old_comments.id, "
                    + "old_comments.story_id, "
                    + "old_comments.subject,"
                    + " old_comments.date, users.nickname "
                    + "FROM old_comments, users "
                    + "WHERE subject LIKE '" + search + "%' "
                    + "AND old_comments.writer = users.id" 
                    + /* OR comment LIKE '$search%%' */
                    // Yasu: for PostgreSQL
                    // " ORDER BY date DESC LIMIT "
                    // + page * nbOfStories + "," + nbOfStories,
//ChienAn                    " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories,
                    " ORDER BY date DESC LIMIT " + nbOfStories + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
                // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	        ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);

		//ChienAn		
		startNano = System.nanoTime();

            rs = stmt.executeQuery();

		//ChienAn
		endNano = System.nanoTime();
                responseTime = (endNano - startNano)/1000;
                logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;

            comment_table = "old_comments";
          }
          if (!rs.first())
          {
            if (page == 0)
              sp.printHTML("<h2>Sorry, but there is no comment matching <i>"
                  + search + "</i> !</h2>");
            else
            {
              sp
                  .printHTML("<h2>Sorry, but there are no more comments available matching <i>"
                      + search + "</i>.</h2><br>\n");
              sp
                  .printHTML("<p><CENTER>\n<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?search="
                      + URLEncoder.encode(search)
                      + "&type="
                      + type
                      + "&page="
                      + (page - 1)
                      + "&nbOfStories="
                      + nbOfStories
                      + "\">Previous page</a>\n</CENTER>\n");
            }

            sp.printHTMLfooter();
            closeConnection(stmt, conn);

		//ChienAn
		if (responseTimeLogger.isInfoEnabled())
                	responseTimeLogger.info(logMessage);
            return;
          }
          else
          {

            // Print the comment subject and author
            do
            {
              String story_id = rs.getString("story_id");
              String id = rs.getString("id");
              String subject = rs.getString("subject");
              String username = rs.getString("nickname");
              String date = rs.getString("date");

              sp
                  .printHTML("<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.ViewComment?comment_table="
                      + comment_table
                      + "&storyId="
                      + story_id
                      + "&commentId="
                      + id
                      + "&filter=0&display=0\">"
                      + subject
                      + "</a> by "
                      + username + " on " + date + "<br>\n");
            }
            while (rs.next());
          }
        }
        catch (Exception e4)
        {
          sp.printHTML(e4 + "Exception in type==1");
          closeConnection(stmt, conn);

		//ChienAn
		if (responseTimeLogger.isInfoEnabled())
                	responseTimeLogger.info(logMessage);
          return;
        }
      } // if (type == 1)

      if (type == 2)
      { // Look for stories of an author
        try
        {
          stmt = conn
              .prepareStatement("SELECT stories.id, stories.title, "
                  + "stories.date, nickname "
                  + "FROM stories,users WHERE writer=users.id"
                  + " AND "
                  + /*
                  * (users.firstname LIKE '$search%%' 
                  * OR users.lastname LIKE
                  * '$search%%' OR
                  */" users.nickname LIKE '"
                  + search + "%'"
                  // Yasu: for PostgreSQL
                  // + /* ) */" ORDER BY date DESC LIMIT "
                  // + page
                  // * nbOfStories + "," + nbOfStories,
                  + /* ) */" ORDER BY date DESC LIMIT " + nbOfStories
//ChienAn                  + " OFFSET " + page * nbOfStories,
                  + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
              // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	      ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);

		//ChienAn
		long startNano = System.nanoTime();		

          rs = stmt.executeQuery();

		//ChienAn
		long endNano = System.nanoTime();
        	long responseTime = (endNano - startNano)/1000;
        	logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;

          if (!rs.first()) 
          {
            stmt.close();
            stmt = conn
                .prepareStatement("SELECT old_stories.id, old_stories.title,"
                    + " old_stories.date, nickname "
                    + "FROM old_stories,users "
                    + "WHERE writer=users.id AND "
                    + /*
                    * (users.firstname LIKE '$search%%' 
                    * OR users.lastname
                    * LIKE '$search%%' OR
                    */" users.nickname LIKE '"
                    + search + "%'" 
                    // Yasu: for PostgreSQL
                    // + /* ) */" ORDER BY date DESC LIMIT "
                    // + page * nbOfStories + "," + nbOfStories,
                    + /* ) */" ORDER BY date DESC LIMIT " + nbOfStories
//ChienAn                    + " OFFSET " + page * nbOfStories,
                    + " OFFSET " + page * nbOfStories + " /* urlID=" + request.getParameter("urlID") + " */",
                // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	        ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);
		
		//ChienAn
		startNano = System.nanoTime();

            rs = stmt.executeQuery();

		//ChienAn
		endNano = System.nanoTime();
                responseTime = (endNano - startNano)/1000;
                logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;
          }
          if (!rs.first())
          {
            if (page == 0)
              sp
                  .printHTML("<h2>Sorry, but there is no story with author matching <i>"
                      + search + "</i> !</h2>");
            else
            {
              sp
                  .printHTML("<h2>Sorry, but there are no more stories available with author matching <i>$search</i>.</h2><br>\n");
              sp
                  .printHTML("<p><CENTER>\n<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?search="
                      + URLEncoder.encode(search)
                      + "&type="
                      + type
                      + "&page="
                      + (page - 1)
                      + "&nbOfStories="
                      + nbOfStories
                      + "\">Previous page</a>\n</CENTER>\n");
            }

            sp.printHTMLfooter();
            closeConnection(stmt, conn);

		//ChienAn
		if (responseTimeLogger.isInfoEnabled())
                        responseTimeLogger.info(logMessage);
            return;
          }
        }
        catch (Exception e6)
        {
          sp.printHTML(e6 + "Exception in type==2");
          closeConnection(stmt, conn);
	
		//ChienAn
		if (responseTimeLogger.isInfoEnabled())
                        responseTimeLogger.info(logMessage);
          return;
        }
      }// if (type == 2)

      try
      {
        if (type != 1)
        {

          // Print the story titles and author
          do
          {
            String id = rs.getString("id");
            String date = rs.getString("date");
            title = rs.getString("title");

            String username = rs.getString("nickname");
            sp
                .printHTML("<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.ViewStory?storyId="
                    + id
                    + "\">"
                    + title
                    + "</a> by "
                    + username
                    + " on "
                    + date + "<br>\n");
          }
          while (rs.next());
        }

        if (page == 0)
          sp
              .printHTML("<p><CENTER>\n<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?search="
                  + URLEncoder.encode(search)
                  + "&type="
                  + type
                  + "&page="
                  + (page + 1)
                  + "&nbOfStories="
                  + nbOfStories
                  + "\">Next page</a>\n</CENTER>\n");
        else
          sp
              .printHTML("<p><CENTER>\n<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?search="
                  + URLEncoder.encode(search)
                  + "&type="
                  + type
                  + "&page="
                  + (page - 1)
                  + "&nbOfStories="
                  + nbOfStories
                  + "\">Previous page</a>\n&nbsp&nbsp&nbsp"
                  + "<a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.Search?category="
                  + search
                  + "="
                  + URLEncoder.encode(search)
                  + "&type="
                  + type
                  + "&page="
                  + (page + 1)
                  + "&nbOfStories="
                  + nbOfStories
                  + "\">Next page</a>\n\n</CENTER>\n");
      }
      catch (Exception e7)
      {
        sp.printHTML(e7 + "Exception in type!=1");
      }
      closeConnection(stmt, conn);
    } // end else

    // We do not need to do a closeConnection() here as a connection
    // is only grabbed in a large if () clause above and we take care
    // of closing it.
    sp.printHTMLfooter();
	
	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                        responseTimeLogger.info(logMessage);
  }

  public void doPost(HttpServletRequest request, HttpServletResponse response)
      throws IOException, ServletException
  {
    doGet(request, response);
  }

}
