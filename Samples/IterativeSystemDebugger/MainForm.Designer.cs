﻿namespace IterativeSystemDebugger
{
    partial class MainForm
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.menuStrip1 = new System.Windows.Forms.MenuStrip();
            this.debuggersToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.erosionSimToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.referenceCFDToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.customCFDToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.menuStrip1.SuspendLayout();
            this.SuspendLayout();
            // 
            // menuStrip1
            // 
            this.menuStrip1.Items.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.debuggersToolStripMenuItem});
            this.menuStrip1.Location = new System.Drawing.Point(0, 0);
            this.menuStrip1.Name = "menuStrip1";
            this.menuStrip1.Size = new System.Drawing.Size(284, 24);
            this.menuStrip1.TabIndex = 0;
            this.menuStrip1.Text = "menuStrip1";
            // 
            // debuggersToolStripMenuItem
            // 
            this.debuggersToolStripMenuItem.DropDownItems.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.erosionSimToolStripMenuItem,
            this.customCFDToolStripMenuItem,
            this.referenceCFDToolStripMenuItem});
            this.debuggersToolStripMenuItem.Name = "debuggersToolStripMenuItem";
            this.debuggersToolStripMenuItem.Size = new System.Drawing.Size(76, 20);
            this.debuggersToolStripMenuItem.Text = "Debuggers";
            // 
            // erosionSimToolStripMenuItem
            // 
            this.erosionSimToolStripMenuItem.Name = "erosionSimToolStripMenuItem";
            this.erosionSimToolStripMenuItem.Size = new System.Drawing.Size(160, 22);
            this.erosionSimToolStripMenuItem.Text = "&Erosion Sim...";
            this.erosionSimToolStripMenuItem.Click += new System.EventHandler(this.erosionSimToolStripMenuItem_Click);
            // 
            // referenceCFDToolStripMenuItem
            // 
            this.referenceCFDToolStripMenuItem.Name = "referenceCFDToolStripMenuItem";
            this.referenceCFDToolStripMenuItem.Size = new System.Drawing.Size(160, 22);
            this.referenceCFDToolStripMenuItem.Text = "Reference CFD...";
            this.referenceCFDToolStripMenuItem.Click += new System.EventHandler(this.referenceCFDToolStripMenuItem_Click);
            // 
            // customCFDToolStripMenuItem
            // 
            this.customCFDToolStripMenuItem.Name = "customCFDToolStripMenuItem";
            this.customCFDToolStripMenuItem.Size = new System.Drawing.Size(160, 22);
            this.customCFDToolStripMenuItem.Text = "Custom CFD...";
            this.customCFDToolStripMenuItem.Click += new System.EventHandler(this.customCFDToolStripMenuItem_Click);
            // 
            // MainForm
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(284, 261);
            this.Controls.Add(this.menuStrip1);
            this.MainMenuStrip = this.menuStrip1;
            this.Name = "MainForm";
            this.Text = "Iterative System Host";
            this.menuStrip1.ResumeLayout(false);
            this.menuStrip1.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.MenuStrip menuStrip1;
        private System.Windows.Forms.ToolStripMenuItem debuggersToolStripMenuItem;
        private System.Windows.Forms.ToolStripMenuItem erosionSimToolStripMenuItem;
        private System.Windows.Forms.ToolStripMenuItem referenceCFDToolStripMenuItem;
        private System.Windows.Forms.ToolStripMenuItem customCFDToolStripMenuItem;
    }
}
