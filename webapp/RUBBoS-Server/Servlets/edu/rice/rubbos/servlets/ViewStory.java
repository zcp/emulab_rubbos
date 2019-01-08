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
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;

import javax.servlet.ServletException;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

//ChienAn
import org.apache.log4j.Logger;

/**
 * Builds the html page with the list of all categories and provides links to
 * browse all items in a category or items in a category for a given region
 */
public class ViewStory extends RubbosHttpServlet
{
  //ChienAn
  static Logger responseTimeLogger = Logger.getLogger(ViewStory.class);

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

    String            title = null, body = null;
    String            date = null, username = null;
    int               storyId = 0;
    int               filter = 0;
    int               display;
    ResultSet         rs = null;
    String            comment_table = null;

    String            storyIdtest  = request.getParameter("storyId");
    String            filterString = request.getParameter("filter");

    sp = new ServletPrinter(response, "ViewStory");

    if (storyIdtest == null)
    {
      sp.printHTML("You must provide a story identifier!<br>");
      return;
    }

    storyId = (Integer.valueOf(request.getParameter("storyId"))).intValue();

    if (filterString != null)
    {
      filter = (Integer.valueOf(filterString)).intValue();
    }

    conn = getRConnection();

    try
    {
      stmt = conn.prepareStatement("SELECT stories.id, "
          + "stories.title, " 
          + "stories.body, stories.date, "
          + "users.nickname FROM stories, users" 
          + " WHERE stories.id=" + storyId
//ChienAn          + " AND stories.writer=users.id",
          + " AND stories.writer=users.id" + " /* urlID=" + request.getParameter("urlID") + " */",
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
      sp.printHTML("ERROR: ViewStory query failed" + e);
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
        stmt = conn.prepareStatement("SELECT old_stories.id, "
            + "old_stories.title, "
            + "old_stories.body, old_stories.date, "
            + "users.nickname FROM old_stories, users"
            + " WHERE old_stories.id=" + storyId 
//ChienAn            + " AND old_stories.writer=users.id",
            + " AND old_stories.writer=users.id" + " /* urlID=" + request.getParameter("urlID") + " */",
            // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	    ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);

	//ChienAn
	long startNano = System.nanoTime();

        rs = stmt.executeQuery();

	//ChienAn
	long endNano = System.nanoTime();
	long responseTime = (endNano - startNano)/1000;
	logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;

        comment_table = "old_comments";

      }
      else
      {
        comment_table = "comments";
      }

      if (!rs.first())
      {
        sp
            .printHTML("<h3>ERROR: Sorry, but this story does not exist.</h3><br>\n");
        closeConnection(stmt, conn);
	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
        	responseTimeLogger.info(logMessage);
        return;
      }

      username = rs.getString("nickname");
      date = rs.getString("date");
      title = rs.getString("title");
      body = rs.getString("body");
    }

    catch (Exception e)
    {
      sp.printHTML("Exception viewing story " + e + "<br>");
      closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
        	responseTimeLogger.info(logMessage);
      return;
    }

    sp.printHTMLheader("RUBBoS: Viewing story " + title);
    sp.printHTMLHighlighted(title);
    sp.printHTML("Posted by " + username + " on " + date + "<br>\n");
    sp.printHTML(body + "<br>\n");
    sp
        .printHTML("<p><center><a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.PostComment?comment_table="
            + comment_table
            + "&storyId="
            + storyId
            + "&parent=0\">Post a comment on this story</a></center><p>");

    // Display filter chooser header
    sp.printHTML("<br><hr><br>");
    display = 1;
    int commentId = 0;

    try
    {
	//ChienAn
	StringBuilder logTmpMessage = new StringBuilder();
//      Comment.displayFilterChooser(conn, sp, commentId, storyIdtest,
//          comment_table, display, filter);
      Comment.displayFilterChooser(conn, sp, commentId, storyIdtest,
          comment_table, display, filter, request.getParameter("urlID"), logTmpMessage);
        logMessage += logTmpMessage.toString();
    }
    catch (Exception e)
    {
      sp.printHTML("Failed to execute Query for ViewStory: " + e);
      closeConnection(stmt, conn);
	
	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
        	responseTimeLogger.info(logMessage);
      return;
    }

    
    try
    {
	//ChienAn
	StringBuilder logTmpMessage = new StringBuilder();
//      Comment.fetchAndDisplay(sp, conn, comment_table, filter, commentId, display, storyIdtest, 0);
      Comment.fetchAndDisplay(sp, conn, comment_table, filter, commentId, display, storyIdtest, 0, request.getParameter("urlID"), logTmpMessage);
	logMessage += logTmpMessage.toString();
    } 
    catch (Exception e)
    {
      sp.printHTML("Failed to execute Query for ViewStory: " + e);
      closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
        	responseTimeLogger.info(logMessage);
      return;
    } 
    
    closeConnection(stmt, conn);

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
