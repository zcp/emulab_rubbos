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

public class SubmitStory extends RubbosHttpServlet
{
	//ChienAn
	static Logger responseTimeLogger = Logger.getLogger(SubmitStory.class);

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
    ResultSet         rs = null;

    sp = new ServletPrinter(response, "Submit Story");
    sp.printHTMLheader("RUBBoS: Story submission");
    sp.printHTML("<center><h2>Submit your incredible story !</h2><br>\n");
    sp
        .printHTML("<form action=\"/rubbos/servlet/edu.rice.rubbos.servlets.StoreStory\" method=POST>\n"
            + "<center><table>\n"
            + "<tr><td><b>Nickname</b><td><input type=text size=20 name=nickname>\n"
            + "<tr><td><b>Password</b><td><input type=text size=20 name=password>\n"
            + "<tr><td><b>Story title</b><td><input type=text size=100 name=title>\n"
            + "<tr><td><b>Category</b><td><SELECT name=category>\n");

    conn = getRConnection();

    // int storyId =
    // (Integer.valueOf(request.getParameter("storyId"))).intValue();

    /*
     * if (storyId == 0) { sp.printHTML( " <h3> You must provide a story
     * identifier ! <br></h3> "); return; }
     */

    try
    {
//ChienAn      stmt = conn.prepareStatement("SELECT * FROM categories",
      stmt = conn.prepareStatement("SELECT * FROM categories" + " /* urlID=" + request.getParameter("urlID") + " */",
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
      sp.printHTML(" Failed to execute Query for SubmitStory: " + e);
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
        sp
            .printHTML("<h3>ERROR: Sorry, but this story does not exist.</h3><br>");
        closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);
        return;
      }

      //Add story to database

      String Name;
      int Id;
      do
      {
        Name = rs.getString("name");
        Id = rs.getInt("id");
        sp.printHTML("<OPTION value=\"" + Id + "\">" + Name + "</OPTION>\n");
      }
      while (rs.next());
    }
    catch (Exception e)
    {
      sp.printHTML("Exception accepting stories: " + e + "<br>");
    }
    
    closeConnection(stmt, conn);


    sp
        .printHTML("</SELECT></table><p><br>\n"
            + "<TEXTAREA rows=\"20\" cols=\"80\" name=\"body\">Write your story here</TEXTAREA><br><p>\n"
            + "<input type=submit value=\"Submit this story now!\"></center><p>\n");
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
